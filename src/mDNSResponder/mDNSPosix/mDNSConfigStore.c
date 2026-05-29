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

/* Forward decl. mDNSResponder's logger; defined in mDNSDebug.c via
 * the LogMsg macro. We use plain fprintf(stderr) here to avoid pulling
 * the mDNS log macro chain through the SCDS subscriber. */

/* Singleton state — the daemon has exactly one SCDS subscriber. */
static SCDynamicStoreRef	g_store;
static dispatch_queue_t		g_queue;

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

/* SCDS callback — every registered key + pattern routes here. */
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
	/* iter 1: log only. Follow-up iters dispatch:
	 *   - Pattern State:/Network/Service/.+/IPv4 → walk interface
	 *     list, open / close mDNS sockets via the same path the
	 *     routing-socket watcher uses today.
	 *   - Key State:/Network/HostNames → re-announce Bonjour records
	 *     under the new LocalHostName.
	 *   - Key State:/Network/Global/IPv4 → re-pick the default-route
	 *     interface for unicast DNS-SD fallback.
	 */
	(void)fflush(stderr);
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
