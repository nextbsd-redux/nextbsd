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
 * iter 1 (this commit): registers the subscriptions and logs every
 * callback. The actual interface-walk + record-republish actions are
 * follow-up iters — landing the subscriber surface first proves the
 * Apple-shape wire-up under our libSystemConfiguration, on the same
 * dispatch infrastructure mDNSResponder already uses for AF_UNIX
 * client requests, without changing the existing routing-socket
 * fallback path. The CI markers MDNS-BOOT-OK / MDNS-ENGINE-OK /
 * MDNS-DNSSD-OK keep working unchanged.
 *
 * The notify_register_dispatch("com.apple.system.hostname") hook the
 * plan describes is intentionally deferred until the libnotify
 * mig_get_special_reply_port hole is resolved (see task #39 Path B
 * memory + the same workaround skipping notify_register_dispatch in
 * hostnamed's vendored set-hostname.c).
 *
 * The routing-socket watcher in mDNSPosix.c stays the authoritative
 * trigger for interface state until iter 2 wires the SCDS callback
 * into the actual interface-list re-walk. Both signals firing in
 * parallel is fine — the interface walk is idempotent.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SCDynamicStore.h>
#include <SystemConfiguration/SCSchemaDefinitions.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* mDNSResponder core API for the re-announce path. */
#include "mDNSEmbeddedAPI.h"		/* mDNS, mDNS_SetFQDN, domainlabel */
#include "DNSCommon.h"			/* mDNS_Lock / mDNS_Unlock macros */

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

/* SCDS callback — every registered key + pattern routes here. We log
 * the change(s) and schedule the debounced recompute. */
static void
mDNSConfigStoreCallback(SCDynamicStoreRef store, CFArrayRef changedKeys,
    void *info)
{
	CFIndex i, count;

	(void)store;
	(void)info;
	if (changedKeys == NULL)
		return;
	count = CFArrayGetCount(changedKeys);
	for (i = 0; i < count; i++) {
		CFStringRef key = CFArrayGetValueAtIndex(changedKeys, i);

		(void)fprintf(stderr, "mDNSResponder mDNSConfigStore: "
		    "key changed -> %s\n", key_kind(key));
	}
	(void)fflush(stderr);
	mDNSConfigStoreDebounce();
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

	/* State:/Network/HostNames — Bonjour LocalHostName surface. */
	key = SCDynamicStoreKeyCreateNetworkGlobalEntity(NULL,
	    kSCDynamicStoreDomainState, CFSTR("HostNames"));
	if (key != NULL) {
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
	    "subscriber up (HostNames + Global/IPv4 + Service/.+/IPv4)\n");
	(void)fflush(stderr);
	return (0);
}
