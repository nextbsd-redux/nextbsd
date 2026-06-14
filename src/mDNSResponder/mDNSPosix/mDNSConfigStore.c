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
 *   - key Setup:/Network/HostNames — fires when prefs_monitor (in
 *     hostnamed) republishes it after an SCPrefs ComputerName change;
 *     mDNSResponder re-adopts the new LocalHostName and re-announces its
 *     Bonjour records under it. (Through iter 4 this watched
 *     State:/Network/HostNames, which nothing ever writes, so the
 *     recompute was dormant; iter 5 / #156 repoints it at the Setup:
 *     key that prefs_monitor actually writes.)
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
 * iter 5 (#156): Bonjour conflict-rename feedback. Two changes. (1) The
 * HostNames watch moves from State: to Setup:/Network/HostNames (the key
 * prefs_monitor actually writes), so a ComputerName/LocalHostName change
 * re-probes the new dot-local name. (2) When mDNSCore's conflict-rename
 * (mDNS_HostNameCallback -> IncrementLabelSuffix) bumps m->hostlabel to
 * "<name>-2", PosixDaemon.c's mDNS_StatusCallback calls
 * mDNSConfigStorePublishResolvedHostName(), which writes the resolved
 * label to State:/Network/HostNames. hostnamed's observer watches that
 * State: key and persists the resolved name to SCPreferences. Setup: =
 * desired, State: = actual-after-conflict — Apple's split. Because mDNS
 * no longer watches State:, the write-back cannot re-trigger our own
 * recompute, and a g_desired_host guard keeps a later recompute from
 * clobbering the conflict suffix back to the un-suffixed desired name.
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

/* Conflict-rename write-back state (#156). g_desired_host is the last
 * LocalHostName we adopted from Setup: (the user/prefs "desired" name);
 * mDNSConfigStoreRecompute compares against THIS, not m->hostlabel, so a
 * conflict-incremented hostlabel ("foo-2") is not clobbered back to "foo"
 * on the next unrelated recompute. Seeded in mDNSConfigStoreInit from the
 * boot hostlabel mDNS_Init adopted via gethostname(3). g_published_host is
 * the last resolved label we wrote to State:/Network/HostNames, making the
 * StatusCallback publisher idempotent (it fires on every successful
 * registration, not just hostname ones). */
static domainlabel	g_desired_host;
static domainlabel	g_published_host;

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
	/* Compare the new desired name against g_desired_host (the last
	 * desired name we applied), NOT against m->hostlabel. After a
	 * conflict-rename m->hostlabel is "foo-2" while the desired name is
	 * still "foo"; comparing against m->hostlabel would reset it to "foo"
	 * and re-trigger the same conflict forever. We only re-adopt + re-probe
	 * when the DESIRED name actually changes (#156). */
	if (!SameDomainLabelCS(g_desired_host.c, new_host.c)) {
		g_desired_host = new_host;
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

/* Convert a domainlabel (length-prefixed, label->c[0] = length) to a
 * NUL-terminated C string. */
static void
label_to_cstr(const domainlabel *label, char *out, size_t outsz)
{
	size_t n = label->c[0];

	if (outsz == 0)
		return;
	if (n > outsz - 1)
		n = outsz - 1;
	(void)memcpy(out, &label->c[1], n);
	out[n] = '\0';
}

/* Write the conflict-resolved LocalHostName to State:/Network/HostNames.
 * Apple's Setup:/State: split: Setup: carries the user/prefs DESIRED name
 * (owned by prefs_monitor); State: carries the ACTUAL name now in use after
 * mDNSResponder's conflict-rename. hostnamed's observer watches the State:
 * key and persists the resolved name back to SCPreferences (#156). */
static void
write_state_local_hostname(SCDynamicStoreRef store, const char *label)
{
	CFStringRef key, val;
	CFMutableDictionaryRef dict;

	if (store == NULL)
		return;
	key = SCDynamicStoreKeyCreateNetworkGlobalEntity(NULL,
	    kSCDynamicStoreDomainState, CFSTR("HostNames"));
	if (key == NULL)
		return;
	val = CFStringCreateWithCString(NULL, label, kCFStringEncodingUTF8);
	dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (val != NULL && dict != NULL) {
		CFDictionarySetValue(dict, kSCPropNetLocalHostName, val);
		if (!SCDynamicStoreSetValue(store, key, dict))
			(void)fprintf(stderr, "mDNSResponder mDNSConfigStore: "
			    "SetValue(State:/Network/HostNames) failed\n");
	}
	if (dict != NULL) CFRelease(dict);
	if (val != NULL) CFRelease(val);
	CFRelease(key);
}

/* Publish mDNSResponder's conflict-resolved dot-local host name to the
 * SCDynamicStore so hostnamed can persist it (#156). Called from
 * PosixDaemon.c's mDNS_StatusCallback on every successful registration
 * (mStatus_NoError); the guards below make it a no-op unless a real
 * conflict-rename produced a hostlabel that differs from the desired name
 * AND we have not already published that exact label.
 *
 * Runs on the mDNS-core MAIN thread (the StatusCallback caller). The only
 * shared object it touches is g_store via SCDynamicStoreSetValue — a
 * synchronous configd IPC independent of g_store's notification dispatch
 * queue, so it is safe from this thread. */
mDNSexport void
mDNSConfigStorePublishResolvedHostName(void)
{
	mDNS *const m = &mDNSStorage;
	char cur[MAX_DOMAIN_LABEL + 1];

	if (g_store == NULL || m->hostlabel.c[0] == 0)
		return;
	/* No conflict-rename: hostlabel still equals the desired name we
	 * adopted from Setup: (seeded from the boot hostlabel). Nothing to do. */
	if (SameDomainLabelCS(m->hostlabel.c, g_desired_host.c))
		return;
	/* Already published this exact resolved label — idempotent across the
	 * many NoError callbacks a single rename produces. */
	if (SameDomainLabelCS(m->hostlabel.c, g_published_host.c))
		return;
	g_published_host = m->hostlabel;
	label_to_cstr(&m->hostlabel, cur, sizeof(cur));
	write_state_local_hostname(g_store, cur);
	(void)fprintf(stderr, "mDNSResponder MDNS-RENAME-PUBLISHED: "
	    "conflict-resolved LocalHostName='%s' -> State:/Network/HostNames "
	    "(desired differed; hostnamed will persist)\n", cur);
	(void)fflush(stderr);
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

/* SCDS callback — every registered key + pattern routes here. We log the
 * change(s), ALWAYS arm the debounced hostname recompute, and wake the main
 * thread to re-walk the interface list when an interface / service IPv4 key
 * changed.
 *
 * The recompute is armed unconditionally rather than only on the exact
 * HostNames key because in our configd the Setup:/Network/HostNames
 * change-notification is not reliably delivered to this subscriber, while
 * the frequent State:/Network/.../IPv4 churn (DHCP renewals) is. Gating the
 * recompute on the HostNames key therefore left it dormant and
 * mDNSResponder never re-adopted a changed LocalHostName (#156). The
 * recompute is idempotent — the g_desired_host guard makes it a no-op
 * unless the desired name actually changed — so riding the IPv4 churn is
 * cheap and mirrors how the vendored set-hostname.c engine already relies
 * on the same churn to re-read its inputs. */
static void
mDNSConfigStoreCallback(SCDynamicStoreRef store, CFArrayRef changedKeys,
    void *info)
{
	CFIndex i, count;
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

		/* Anything that is not the exact HostNames key is an interface /
		 * service IPv4 change (State:/Network/Global/IPv4 or the
		 * State:/Network/Service/.+/IPv4 pattern) and drives the
		 * interface re-walk. The hostname recompute is armed
		 * unconditionally below, so no separate hostname flag is kept. */
		if (!(g_hostnames_key[0] != '\0' &&
		    strcmp(kk, g_hostnames_key) == 0))
			interface_changed = TRUE;
	}
	(void)fflush(stderr);

	/* Always arm the hostname recompute (see the function comment): the
	 * Setup: HostNames notify is unreliable here, so we ride the IPv4
	 * churn; the recompute is idempotent under the g_desired_host guard. */
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

	/* Setup:/Network/HostNames — the DESIRED Bonjour LocalHostName,
	 * written by prefs_monitor and read by read_local_hostname. Watching
	 * this (not the State: key, which nothing writes) is what makes a
	 * ComputerName/LocalHostName change re-probe the new dot-local name
	 * (#156). Cache its string form so the callback can tell the hostname
	 * path from the interface path (see g_hostnames_key). */
	key = SCDynamicStoreKeyCreateHostNames(NULL);
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

	/* Seed the desired-name guard with the boot hostlabel mDNS_Init just
	 * adopted from gethostname(3) (PosixDaemon.c calls us right after
	 * mDNS_Init, before the event loop concludes the first probe). The
	 * conflict-rename publisher compares m->hostlabel against this, so it
	 * fires only when a real rename changes the name — never for the
	 * un-conflicted boot name. */
	g_desired_host = mDNSStorage.hostlabel;

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
