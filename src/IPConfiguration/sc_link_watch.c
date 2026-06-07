/*
 * sc_link_watch.c — ipconfigd SCDynamicStore link-state watcher.
 *
 * Watches State:/Network/Interface/<if>/Link (published by the
 * KernelEventMonitor daemon from PF_ROUTE link-state changes). When an
 * interface's Link dict turns { Active : true }, schedule the caller's
 * callback to start DHCP on it. This is the Apple-shaped trigger
 * (KernelEventMonitor -> SCDynamicStore -> IPConfiguration) that
 * replaces ipconfigd's removed hwregd attach subscription.
 *
 * Threading: the SC change callout runs on a dedicated serial delivery
 * queue (under SCDynamicStoreSetDispatchQueue). To keep that queue
 * free, the callout never runs DHCP itself — it dispatches each
 * link-up to the global concurrent queue, where the callback may block
 * in the DHCP + lease loop. The same scheduling path serves the
 * one-shot initial scan, so a link already Active before we subscribed
 * still fires. Concurrent/duplicate fires are the callback's problem
 * to dedupe (ipconfigd guards on bound-state + a dhcp-in-progress
 * flag).
 *
 * Single CF translation unit, like sc_publish.c — the rest of the
 * daemon stays plain C.
 */
#include "sc_link_watch.h"

#include <SystemConfiguration/SCDynamicStore.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>

#include <net/if.h>		/* IFNAMSIZ */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sc_link_watch_cb	g_cb;
static uint32_t		g_lease_cap_secs;

/* The watch pattern: every interface's Link entity. POSIX ERE — configd
 * anchors it ^...$. [^/]+ matches one path component (the ifname). */
#define LINK_PATTERN	"State:/Network/Interface/[^/]+/Link"

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "ipconfigd[link] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

static CFStringRef
mkstr(const char *s)
{
	return (CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8));
}

/* Trampoline payload — heap-allocated, freed by the trampoline. */
struct link_event {
	char		ifname[IFNAMSIZ];
	int		active;
	uint32_t	lease_cap_secs;
};

static void
run_link_trampoline(void *arg)
{
	struct link_event *ev = arg;

	if (g_cb != NULL)
		g_cb(ev->ifname, ev->active, ev->lease_cap_secs);
	free(ev);
}

/* Schedule the caller's link-event callback for `ifname` on the global queue. */
static void
schedule_link_event(const char *ifname, int active)
{
	struct link_event *ev;

	ev = calloc(1, sizeof(*ev));
	if (ev == NULL)
		return;
	(void)strlcpy(ev->ifname, ifname, sizeof(ev->ifname));
	ev->active = active;
	ev->lease_cap_secs = g_lease_cap_secs;
	dispatch_async_f(dispatch_get_global_queue(
	    DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ev, run_link_trampoline);
}

/*
 * Pull "State:/Network/Interface/<if>/Link"'s Active flag out of the store
 * and schedule the caller's callback for <if>, passing the link state.
 * Fired for BOTH up and down so the callback can admin-up a newly-seen
 * interface whose link is still down (the late-arriving-NIC case, #219) and
 * DHCP it once it links. Extracts the ifname from the key text rather than the
 * dict, so it works for both the change callout and the initial scan. lo0 is
 * filtered here so the callback never sees loopback.
 */
static void
handle_link_key(SCDynamicStoreRef store, CFStringRef key)
{
	char keybuf[128];
	char ifname[IFNAMSIZ];
	const char *prefix = "State:/Network/Interface/";
	const char *p, *slash;
	size_t n;
	CFDictionaryRef dict;
	CFBooleanRef active;
	int is_active;

	if (!CFStringGetCString(key, keybuf, sizeof(keybuf),
	    kCFStringEncodingUTF8))
		return;
	if (strncmp(keybuf, prefix, strlen(prefix)) != 0)
		return;
	p = keybuf + strlen(prefix);
	slash = strchr(p, '/');
	if (slash == NULL)
		return;
	n = (size_t)(slash - p);
	if (n == 0 || n >= sizeof(ifname))
		return;
	(void)memcpy(ifname, p, n);
	ifname[n] = '\0';

	if (strncmp(ifname, "lo", 2) == 0)	/* loopback never DHCPs */
		return;

	dict = SCDynamicStoreCopyValue(store, key);
	if (dict == NULL)
		return;
	if (CFGetTypeID(dict) != CFDictionaryGetTypeID()) {
		CFRelease(dict);
		return;
	}
	active = CFDictionaryGetValue(dict, CFSTR("Active"));
	is_active = (active != NULL &&
	    CFGetTypeID(active) == CFBooleanGetTypeID() &&
	    CFBooleanGetValue(active));
	if (is_active)
		xlog("IPCFG-LINK-UP: %s link Active — scheduling DHCP", ifname);
	else
		xlog("IPCFG-LINK-SEEN: %s present, link down — scheduling "
		    "admin-up", ifname);
	schedule_link_event(ifname, is_active);
	CFRelease(dict);
}

/* SCDynamicStore change callout — one or more watched keys changed. */
static void
link_changed(SCDynamicStoreRef store, CFArrayRef changedKeys, void *info)
{
	CFIndex i, count;

	(void)info;
	count = CFArrayGetCount(changedKeys);
	for (i = 0; i < count; i++)
		handle_link_key(store, CFArrayGetValueAtIndex(changedKeys, i));
}

int
sc_link_watch_start(sc_link_watch_cb cb, uint32_t lease_cap_secs)
{
	SCDynamicStoreContext ctx;
	SCDynamicStoreRef store;
	dispatch_queue_t queue;
	CFStringRef name, pattern;
	CFArrayRef patterns, existing;
	const void *pvals[1];

	g_cb = cb;
	g_lease_cap_secs = lease_cap_secs;

	(void)memset(&ctx, 0, sizeof(ctx));
	name = mkstr("ipconfigd-linkwatch");
	/*
	 * Retry the session open: ipconfigd and configd are both RunAtLoad
	 * with no ordering guarantee, so the first create can race configd's
	 * bootstrap check-in. Without the retry a lost race would silently
	 * leave the daemon with no DHCP trigger at all.
	 */
	{
		int tries;

		store = NULL;
		for (tries = 0; tries < 60; tries++) {
			store = SCDynamicStoreCreate(NULL, name, link_changed,
			    &ctx);
			if (store != NULL)
				break;
			(void)sleep(1);
		}
	}
	if (name != NULL)
		CFRelease(name);
	if (store == NULL) {
		xlog("IPCFG-LINK-FAIL: SCDynamicStoreCreate: %s",
		    SCErrorString(SCError()));
		return (-1);
	}

	pattern = mkstr(LINK_PATTERN);
	pvals[0] = pattern;
	patterns = CFArrayCreate(NULL, pvals, 1, &kCFTypeArrayCallBacks);
	if (!SCDynamicStoreSetNotificationKeys(store, NULL, patterns)) {
		xlog("IPCFG-LINK-FAIL: SetNotificationKeys: %s",
		    SCErrorString(SCError()));
		CFRelease(patterns);
		CFRelease(pattern);
		CFRelease(store);
		return (-1);
	}

	queue = dispatch_queue_create("com.apple.ipconfigd.linkwatch", NULL);
	if (!SCDynamicStoreSetDispatchQueue(store, queue)) {
		xlog("IPCFG-LINK-FAIL: SetDispatchQueue: %s",
		    SCErrorString(SCError()));
		dispatch_release(queue);
		CFRelease(patterns);
		CFRelease(pattern);
		CFRelease(store);
		return (-1);
	}
	xlog("IPCFG-LINK-WATCH-OK: watching %s", LINK_PATTERN);

	/*
	 * Initial scan: a link that was already Active before the watch
	 * registered won't generate a change event, so sweep matching keys
	 * once and fire for any that are already up. KernelEventMonitor's
	 * own startup snapshot makes this the common path in CI (SLIRP
	 * link is up immediately).
	 */
	existing = SCDynamicStoreCopyKeyList(store, pattern);
	if (existing != NULL) {
		CFIndex i, count = CFArrayGetCount(existing);

		for (i = 0; i < count; i++)
			handle_link_key(store,
			    CFArrayGetValueAtIndex(existing, i));
		CFRelease(existing);
	}

	CFRelease(patterns);
	CFRelease(pattern);
	/*
	 * Intentionally leak `store` + `queue` for the daemon's lifetime —
	 * the watch must stay live until exit; there is no stop path.
	 */
	return (0);
}
