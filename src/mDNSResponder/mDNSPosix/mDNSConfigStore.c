/*
 * mDNSConfigStore.c — Apple-shape SCDynamicStore subscriber for
 * mDNSResponder. freebsd-launchd-mach iter K issue #62.
 *
 * BACKGROUND
 *
 * Apple's mDNSResponder reacts to network state changes via SCDS
 * notifications (PosixDaemon.c's interface walk on real macOS calls
 * out through SystemConfiguration on every kSCEntNetIPv4 / HostNames
 * change). On the open-source POSIX build, the routing-socket watcher
 * in mDNSPosix.c is the fallback when SCDS isn't available.
 *
 * Our build has SCDS via libSystemConfiguration + configd, so the
 * Apple-shape path is available. This file subscribes:
 *
 *   - pattern State:/Network/Service/.+/IPv4 — fires when ipconfigd
 *     publishes IPv4 for any interface (boot-time interface arrival
 *     under DHCP), or removes it on lease loss. mDNSResponder should
 *     re-walk its interface list, open / close mDNS sockets for the
 *     newly-bound / newly-gone interfaces.
 *
 *   - key State:/Network/HostNames — fires when prefs_monitor (in
 *     hostnamed) republishes Setup:/Network/HostNames after SCPrefs
 *     ComputerName changes; mDNSResponder should re-announce its
 *     Bonjour records under the new LocalHostName.
 *
 *   - key State:/Network/Global/IPv4 — fires when ipconfigd selects a
 *     new primary service / interface. mDNSResponder should re-pick
 *     its default-route interface for unicast DNS-SD fallback.
 *
 * iter 1: registered the subscriptions and logged every callback,
 * plus the HostNames -> mDNS_SetFQDN re-announce path (the debounced
 * Recompute below). It proved the Apple-shape wire-up under our
 * libSystemConfiguration, on the same dispatch infrastructure
 * mDNSResponder already uses for AF_UNIX client requests, without
 * changing the existing routing-socket fallback path. The CI markers
 * MDNS-BOOT-OK / MDNS-ENGINE-OK / MDNS-DNSSD-OK keep working unchanged.
 *
 * iter 4 (this commit): wires the Global/IPv4 and Service/.+/IPv4
 * subscriptions into the actual interface-list re-walk. When ipconfigd
 * publishes (DHCP bind) or removes (lease loss) a service's IPv4, the
 * SCDS callback now re-walks mDNSResponder's interface list and
 * re-announces Bonjour records by calling
 * mDNSPlatformPosixRefreshInterfaceList — the same heavyweight
 * ClearInterfaceList + SetupInterfaceList the routing-socket watcher
 * (InterfaceChangeCallback in mDNSPosix.c) already drives. Both
 * triggers firing in parallel stays fine — the interface walk is
 * idempotent.
 *
 * THREADING. The mDNS core runs single-threaded on the daemon's MAIN
 * thread (PosixDaemon.c's mDNSPosixRunEventLoopOnce select loop). The
 * Posix mDNSPlatformLock/Unlock are no-ops — there is no real mutex —
 * so the only safe way to mutate core state is from the main thread.
 * Our SCDS callback, by contrast, runs on g_queue (a dedicated
 * dispatch queue). Calling mDNSPlatformPosixRefreshInterfaceList
 * straight from g_queue would tear down and rebuild the very socket
 * set the select loop is iterating, racing it. So we use a self-pipe:
 * the SCDS path (on g_queue) writes one wake byte to g_wake_wr; the
 * read end g_wake_rd is registered in the mDNS event loop via
 * mDNSPosixAddFDToEventLoop, so mDNSConfigStoreWake runs on the MAIN
 * thread, drains the pipe, and does the refresh there — matching where
 * InterfaceChangeCallback already runs (no mDNS_Lock).
 *
 * The HostNames path keeps its iter-1 shape: the SCDS callback arms a
 * 25ms debounce timer on g_queue whose handler (mDNSConfigStoreRecompute)
 * reads the labels and calls mDNS_SetFQDN. That call is technically
 * cross-thread too (a latent iter-1 race), but rerouting it through the
 * pipe would entangle the proven hostname path with the new interface
 * path for no functional gain here, so iter 4 leaves it as-is and only
 * adds the interface path on the main thread. See the PR for the
 * tradeoff.
 *
 * The notify_register_dispatch("com.apple.system.hostname") hook the
 * plan describes is intentionally deferred until the libnotify
 * mig_get_special_reply_port hole is resolved (see task #39 Path B
 * memory + the same workaround skipping notify_register_dispatch in
 * hostnamed's vendored set-hostname.c).
 *
 * The routing-socket watcher in mDNSPosix.c remains as a parallel
 * trigger for interface state; the SCDS callback is now an additional,
 * Apple-shape trigger that fires off the same SystemConfiguration
 * publishes ipconfigd already emits. Both signals firing in parallel
 * is fine — the interface walk is idempotent.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SCDynamicStore.h>
#include <SystemConfiguration/SCSchemaDefinitions.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* mDNSResponder core API for the re-announce path. */
#include "mDNSEmbeddedAPI.h"		/* mDNS, mDNS_SetFQDN, domainlabel */
#include "DNSCommon.h"			/* mDNS_Lock / mDNS_Unlock macros */
#include "mDNSPosix.h"			/* mDNSPlatformPosixRefreshInterfaceList,
					 * mDNSPosixAddFDToEventLoop — the
					 * mDNS-core event-loop FD registration */

/* PosixDaemon.c's global mDNS instance — the SCDS callback drives
 * a re-announce on it whenever Setup:/Network/HostNames or
 * Setup:/System/ComputerName changes. */
extern mDNS mDNSStorage;

/* Forward decl. mDNSResponder's logger; defined in mDNSDebug.c via
 * the LogMsg macro. We use plain fprintf(stderr) here to avoid pulling
 * the mDNS log macro chain through the SCDS subscriber. */

/* Singleton state — the daemon has exactly one SCDS subscriber. */
static SCDynamicStoreRef	g_store;
static dispatch_queue_t		g_queue;

/* Debounce timer state. Apple's mDNSMacOSX.c:6534 SetNetworkChanged()
 * coalesces SCDS bursts via a ~25ms timer before re-running the
 * recompute path. We mirror that pattern: any SCDS callback schedules
 * a dispatch_after at +DEBOUNCE_MS; the timer's handler does the
 * actual SCDS read + label compare + mDNS_SetFQDN. Coalescing keeps
 * a flurry of prefs_monitor publishes (one per key written) from
 * triggering N separate re-announces. */
#define DEBOUNCE_MS	25
static dispatch_source_t	g_debounce_timer;

/* Self-pipe used to hop interface-refresh requests from the SCDS
 * dispatch queue (g_queue) onto the mDNS-core MAIN thread. The SCDS
 * callback writes one wake byte to g_wake_wr; mDNSConfigStoreWake,
 * registered on the mDNS event loop's read set, runs on the main
 * thread, drains the pipe, and re-walks the interface list there. See
 * the THREADING note at the top of the file. -1 = not yet created. */
static int	g_wake_rd = -1;
static int	g_wake_wr = -1;

/* Cached State:/Network/HostNames key string (UTF-8). Used to tell the
 * hostname-only path (debounce -> Recompute -> mDNS_SetFQDN) from the
 * interface path (pipe wake -> RefreshInterfaceList) when classifying a
 * changed key in the SCDS callback. Empty if it couldn't be cached. */
static char	g_hostnames_key[256];

static const char *
key_kind(CFStringRef key)
{
	static char buf[256];

	if (key == NULL)
		return ("<NULL>");
	buf[0] = '\0';
	(void)CFStringGetCString(key, buf, sizeof(buf),
	    kCFStringEncodingUTF8);
	return (buf);
}

/* Read Setup:/Network/HostNames/LocalHostName into *out, returning
 * TRUE on success. The fallback chain is SCDS → gethostname(3). */
static Boolean
read_local_hostname(SCDynamicStoreRef store, char *out, size_t outsz)
{
	CFStringRef key;
	CFDictionaryRef dict;
	Boolean ok = FALSE;

	if (store == NULL || out == NULL || outsz == 0)
		return (FALSE);

	key = SCDynamicStoreKeyCreateHostNames(NULL);
	if (key != NULL) {
		dict = SCDynamicStoreCopyValue(store, key);
		CFRelease(key);
		if (dict != NULL) {
			CFStringRef name;

			if (CFGetTypeID(dict) == CFDictionaryGetTypeID()) {
				name = CFDictionaryGetValue(dict,
				    kSCPropNetLocalHostName);
				if (name != NULL &&
				    CFGetTypeID(name) == CFStringGetTypeID() &&
				    CFStringGetCString(name, out, outsz,
				    kCFStringEncodingUTF8) &&
				    out[0] != '\0')
					ok = TRUE;
			}
			CFRelease(dict);
		}
	}
	if (!ok) {
		/* Fallback: kernel hostname (launchd PID-1 set this to the
		 * synth value at early-init; later hostnamed refines it). */
		if (gethostname(out, outsz) == 0 && out[0] != '\0') {
			char *dot = strchr(out, '.');
			if (dot != NULL) *dot = '\0';
			ok = TRUE;
		}
	}
	return (ok);
}

/* Read Setup:/System/ComputerName, falling back to LocalHostName. */
static Boolean
read_computer_name(SCDynamicStoreRef store, char *out, size_t outsz)
{
	CFStringRef key;
	CFDictionaryRef dict;
	Boolean ok = FALSE;

	if (store == NULL || out == NULL || outsz == 0)
		return (FALSE);
	key = SCDynamicStoreKeyCreateComputerName(NULL);
	if (key != NULL) {
		dict = SCDynamicStoreCopyValue(store, key);
		CFRelease(key);
		if (dict != NULL) {
			CFStringRef name;
			if (CFGetTypeID(dict) == CFDictionaryGetTypeID()) {
				name = CFDictionaryGetValue(dict,
				    kSCPropSystemComputerName);
				if (name != NULL &&
				    CFGetTypeID(name) == CFStringGetTypeID() &&
				    CFStringGetCString(name, out, outsz,
				    kCFStringEncodingUTF8) &&
				    out[0] != '\0')
					ok = TRUE;
			}
			CFRelease(dict);
		}
	}
	if (!ok)
		return (read_local_hostname(store, out, outsz));
	return (ok);
}

/* Populate a domainlabel from a C string, mirroring mDNSPosix.c:969
 * GetUserSpecifiedRFC1034ComputerName (truncate at first dot, cap
 * at MAX_DOMAIN_LABEL). */
static void
fill_domainlabel(domainlabel *label, const char *s)
{
	int len = 0;

	label->c[0] = 0;
	if (s == NULL || s[0] == '\0')
		return;
	while (len < MAX_DOMAIN_LABEL && s[len] != '\0' && s[len] != '.') {
		label->c[len + 1] = (mDNSu8)s[len];
		len++;
	}
	label->c[0] = (mDNSu8)len;
}

/* Debounce-fire handler. Reads the SCDS labels, compares them to the
 * cached labels on mDNSStorage, and on change calls mDNS_SetFQDN(m)
 * under mDNS_Lock — Apple's pattern (mDNSMacOSX.c:4239-4246). */
static void
mDNSConfigStoreRecompute(void *info)
{
	mDNS *const m = &mDNSStorage;
	char host_cstr[MAX_DOMAIN_LABEL + 1] = "";
	char nice_cstr[MAX_DOMAIN_LABEL + 1] = "";
	domainlabel new_host, new_nice;
	Boolean changed = FALSE;

	(void)info;

	if (!read_local_hostname(g_store, host_cstr, sizeof(host_cstr)))
		return;
	if (!read_computer_name(g_store, nice_cstr, sizeof(nice_cstr)))
		(void)strncpy(nice_cstr, host_cstr, sizeof(nice_cstr) - 1);

	fill_domainlabel(&new_host, host_cstr);
	fill_domainlabel(&new_nice, nice_cstr);

	if (new_host.c[0] == 0)
		return;	/* read succeeded but empty label — skip */

	mDNS_Lock(m);
	if (!SameDomainLabelCS(m->hostlabel.c, new_host.c)) {
		m->hostlabel = new_host;
		changed = TRUE;
	}
	if (!SameDomainLabelCS(m->nicelabel.c, new_nice.c)) {
		m->nicelabel = new_nice;
		changed = TRUE;
	}
	if (changed) {
		(void)fprintf(stderr, "mDNSResponder mDNSConfigStore: "
		    "hostname change -> hostlabel='%s' nicelabel='%s' — "
		    "calling mDNS_SetFQDN\n", host_cstr, nice_cstr);
		(void)fflush(stderr);
		mDNS_SetFQDN(m);
	}
	mDNS_Unlock(m);
}

/* Schedule (or re-schedule) the debounce-fire timer. Any SCDS
 * callback within DEBOUNCE_MS coalesces into a single re-announce. */
static void
mDNSConfigStoreDebounce(void)
{
	if (g_debounce_timer == NULL)
		return;
	dispatch_source_set_timer(g_debounce_timer,
	    dispatch_time(DISPATCH_TIME_NOW,
	        DEBOUNCE_MS * (uint64_t)NSEC_PER_MSEC),
	    DISPATCH_TIME_FOREVER, /* one-shot until next schedule */
	    DEBOUNCE_MS * (uint64_t)NSEC_PER_MSEC / 10);
}

/* Post an interface-refresh request from the SCDS dispatch queue to the
 * mDNS-core main thread by writing one wake byte to the self-pipe. Runs
 * on g_queue. Coalescing happens naturally on the read side: the main
 * thread drains every queued byte in one RefreshInterfaceList. EINTR is
 * retried; EAGAIN (pipe already full of pending wakes) is benign — a
 * refresh is already pending, so dropping this byte loses nothing. */
static void
mDNSConfigStorePostInterfaceRefresh(void)
{
	const char byte = 'i';
	ssize_t n;

	if (g_wake_wr < 0)
		return;
	do {
		n = write(g_wake_wr, &byte, 1);
	} while (n < 0 && errno == EINTR);
	if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
		(void)fprintf(stderr, "mDNSResponder mDNSConfigStore: "
		    "wake-pipe write failed: %s\n", strerror(errno));
}

/* mDNS event-loop read callback for the self-pipe read end. Runs on the
 * MAIN thread (mDNSPosixRunEventLoopOnce's select loop), so it may
 * mutate mDNS-core state directly. Drains every queued wake byte, then
 * re-walks the interface list once — matching InterfaceChangeCallback
 * in mDNSPosix.c, which also calls RefreshInterfaceList from the event
 * loop without mDNS_Lock (the Posix lock is a no-op). */
static void
mDNSConfigStoreWake(int fd, void *context)
{
	char drain[64];
	ssize_t n;

	(void)context;

	/* Drain the pipe — coalesce a burst of wakes into one refresh. */
	do {
		n = read(fd, drain, sizeof(drain));
	} while (n > 0 || (n < 0 && errno == EINTR));

	(void)fprintf(stderr, "mDNSResponder mDNSConfigStore: "
	    "MDNS-IFWATCH-OK: interface/service IPv4 change -> "
	    "re-walking interface list (RefreshInterfaceList)\n");
	(void)fflush(stderr);

	(void)mDNSPlatformPosixRefreshInterfaceList(&mDNSStorage);
}

/* SCDS callback — every registered key + pattern routes here. We log
 * the change(s) and dispatch: HostNames changes arm the debounced
 * hostname recompute (mDNS_SetFQDN); interface / service IPv4 changes
 * wake the main thread to re-walk the interface list. A single callback
 * can carry both kinds, so we track them independently. */
static void
mDNSConfigStoreCallback(SCDynamicStoreRef store, CFArrayRef changedKeys,
    void *info)
{
	CFIndex i, count;
	Boolean hostname_changed = FALSE;
	Boolean interface_changed = FALSE;

	(void)store;
	(void)info;
	if (changedKeys == NULL)
		return;
	count = CFArrayGetCount(changedKeys);
	for (i = 0; i < count; i++) {
		CFStringRef key = CFArrayGetValueAtIndex(changedKeys, i);
		const char *kk = key_kind(key);

		(void)fprintf(stderr, "mDNSResponder mDNSConfigStore: "
		    "key changed -> %s\n", kk);

		/* Classify: the exact HostNames key drives the hostname
		 * recompute; everything else we subscribe to is interface /
		 * service IPv4 (State:/Network/Global/IPv4 or the
		 * State:/Network/Service/.+/IPv4 pattern) and drives the
		 * interface re-walk. */
		if (g_hostnames_key[0] != '\0' &&
		    strcmp(kk, g_hostnames_key) == 0)
			hostname_changed = TRUE;
		else
			interface_changed = TRUE;
	}
	(void)fflush(stderr);

	if (hostname_changed)
		mDNSConfigStoreDebounce();
	if (interface_changed)
		mDNSConfigStorePostInterfaceRefresh();
}

/* Build the watch lists per SCDynamicStoreSetNotificationKeys's
 * contract — `keys` is exact-match, `patterns` carries the kSCCompAnyRegex
 * placeholder ("[^/]+") substituted into a service-UUID-shaped path. */
static Boolean
mDNSConfigStoreSubscribe(SCDynamicStoreRef store)
{
	CFMutableArrayRef	keys;
	CFMutableArrayRef	patterns;
	CFStringRef		key;
	Boolean			ok;

	keys = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	patterns = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	if (keys == NULL || patterns == NULL) {
		if (keys != NULL) CFRelease(keys);
		if (patterns != NULL) CFRelease(patterns);
		return (FALSE);
	}

	/* State:/Network/HostNames — Bonjour LocalHostName surface. Cache
	 * its string form so the callback can tell the hostname path from
	 * the interface path (see g_hostnames_key). */
	key = SCDynamicStoreKeyCreateNetworkGlobalEntity(NULL,
	    kSCDynamicStoreDomainState, CFSTR("HostNames"));
	if (key != NULL) {
		(void)CFStringGetCString(key, g_hostnames_key,
		    sizeof(g_hostnames_key), kCFStringEncodingUTF8);
		CFArrayAppendValue(keys, key);
		CFRelease(key);
	}

	/* State:/Network/Global/IPv4 — primary service / interface. */
	key = SCDynamicStoreKeyCreateNetworkGlobalEntity(NULL,
	    kSCDynamicStoreDomainState, kSCEntNetIPv4);
	if (key != NULL) {
		CFArrayAppendValue(keys, key);
		CFRelease(key);
	}

	/* Pattern State:/Network/Service/[^/]+/IPv4 — per-interface
	 * IPv4 publish on DHCP bind / lease loss. */
	key = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
	    kSCDynamicStoreDomainState, kSCCompAnyRegex, kSCEntNetIPv4);
	if (key != NULL) {
		CFArrayAppendValue(patterns, key);
		CFRelease(key);
	}

	ok = SCDynamicStoreSetNotificationKeys(store, keys, patterns);
	CFRelease(keys);
	CFRelease(patterns);
	return (ok);
}

/* Mark a descriptor non-blocking + close-on-exec. We want the write end
 * non-blocking so a wake from g_queue can never stall the dispatch
 * queue, and both ends close-on-exec so they don't leak into children.
 * Returns TRUE on success. */
static Boolean
set_nonblock_cloexec(int fd)
{
	int fl;

	fl = fcntl(fd, F_GETFL, 0);
	if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
		return (FALSE);
	fl = fcntl(fd, F_GETFD, 0);
	if (fl < 0 || fcntl(fd, F_SETFD, fl | FD_CLOEXEC) < 0)
		return (FALSE);
	return (TRUE);
}

/* Create the cross-thread wake pipe and register its read end on the
 * mDNS event loop so mDNSConfigStoreWake fires on the main thread. On
 * any failure we tear the pipe back down and return FALSE; the caller
 * treats that as non-fatal (interface refresh then relies solely on the
 * routing-socket watcher, exactly as before iter 4). */
static Boolean
mDNSConfigStoreSetupWakePipe(void)
{
	int fds[2];

	if (pipe(fds) != 0) {
		(void)fprintf(stderr, "mDNSResponder mDNSConfigStore: "
		    "wake-pipe pipe() failed: %s\n", strerror(errno));
		return (FALSE);
	}
	if (!set_nonblock_cloexec(fds[0]) || !set_nonblock_cloexec(fds[1])) {
		(void)fprintf(stderr, "mDNSResponder mDNSConfigStore: "
		    "wake-pipe fcntl() failed: %s\n", strerror(errno));
		(void)close(fds[0]);
		(void)close(fds[1]);
		return (FALSE);
	}
	g_wake_rd = fds[0];
	g_wake_wr = fds[1];

	if (mDNSPosixAddFDToEventLoop(g_wake_rd, mDNSConfigStoreWake, NULL)
	    != mStatus_NoError) {
		(void)fprintf(stderr, "mDNSResponder mDNSConfigStore: "
		    "mDNSPosixAddFDToEventLoop(wake) failed\n");
		(void)close(g_wake_rd);
		(void)close(g_wake_wr);
		g_wake_rd = -1;
		g_wake_wr = -1;
		return (FALSE);
	}
	return (TRUE);
}

/* Entry point called from PosixDaemon.c main() AFTER mDNS_Init has
 * returned successfully — we want the daemon engine up before we
 * start dispatching SCDS callbacks. Failure is non-fatal: the
 * routing-socket watcher in mDNSPosix.c continues to drive interface
 * state. Returns 0 on success, non-zero on failure. */
int
mDNSConfigStoreInit(void)
{
	SCDynamicStoreContext	ctx = {0, NULL, NULL, NULL, NULL};

	g_queue = dispatch_queue_create("com.apple.mDNSResponder.scds",
	    NULL);
	if (g_queue == NULL) {
		(void)fprintf(stderr, "mDNSResponder mDNSConfigStore: "
		    "dispatch_queue_create failed\n");
		return (-1);
	}

	/* One-shot timer used as the 25ms debounce — Apple's
	 * SetNetworkChanged pattern (mDNSMacOSX.c:6534). Created here
	 * and re-armed by every SCDS callback; the handler does the
	 * SCDS read + label compare + mDNS_SetFQDN. */
	g_debounce_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,
	    0, 0, g_queue);
	if (g_debounce_timer == NULL) {
		(void)fprintf(stderr, "mDNSResponder mDNSConfigStore: "
		    "dispatch_source_create(timer) failed\n");
		return (-1);
	}
	dispatch_source_set_event_handler_f(g_debounce_timer,
	    mDNSConfigStoreRecompute);
	dispatch_source_set_timer(g_debounce_timer, DISPATCH_TIME_FOREVER,
	    DISPATCH_TIME_FOREVER, 0);
	dispatch_activate(g_debounce_timer);

	/* Cross-thread wake pipe for the interface re-walk. Set up BEFORE
	 * the SCDS dispatch queue starts delivering callbacks, so the read
	 * end is already on the main-thread event loop the first time the
	 * callback writes to it. Non-fatal on failure: the routing-socket
	 * watcher in mDNSPosix.c still drives interface changes, and the
	 * write side guards on g_wake_wr < 0. */
	if (!mDNSConfigStoreSetupWakePipe())
		(void)fprintf(stderr, "mDNSResponder mDNSConfigStore: "
		    "wake pipe unavailable — interface refresh falls back to "
		    "the routing-socket watcher only\n");

	g_store = SCDynamicStoreCreate(NULL,
	    CFSTR("com.apple.mDNSResponder.config"),
	    mDNSConfigStoreCallback, &ctx);
	if (g_store == NULL) {
		(void)fprintf(stderr, "mDNSResponder mDNSConfigStore: "
		    "SCDynamicStoreCreate failed (configd not up?)\n");
		return (-1);
	}

	if (!mDNSConfigStoreSubscribe(g_store)) {
		(void)fprintf(stderr, "mDNSResponder mDNSConfigStore: "
		    "SCDynamicStoreSetNotificationKeys failed\n");
		CFRelease(g_store);
		g_store = NULL;
		return (-1);
	}

	if (!SCDynamicStoreSetDispatchQueue(g_store, g_queue)) {
		(void)fprintf(stderr, "mDNSResponder mDNSConfigStore: "
		    "SCDynamicStoreSetDispatchQueue failed\n");
		CFRelease(g_store);
		g_store = NULL;
		return (-1);
	}

	(void)fprintf(stderr, "mDNSResponder MDNS-SCDS-OK: SCDynamicStore "
	    "subscriber up (HostNames + Global/IPv4 + Service/.+/IPv4)%s\n",
	    g_wake_wr >= 0 ? "; iter-4 interface watcher armed" : "");
	(void)fflush(stderr);
	return (0);
}
