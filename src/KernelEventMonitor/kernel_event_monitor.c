/*
 * kernel_event_monitor.c — freebsd-launchd-mach KernelEventMonitor.
 *
 * The Mach-IPC track port of Apple's SystemConfiguration
 * KernelEventMonitor plugin (configd's Plugins/KernelEventMonitor).
 * Apple runs it as a configd plugin sharing configd's one event loop
 * over both the configd Mach port and the kernel-event socket. We run
 * it standalone because (a) this repo's configd has no plugin loader
 * and (b) stock FreeBSD has no EVFILT_MACHPORT, so configd's single
 * mach_msg loop can't multiplex a Mach port and a route-socket fd in
 * one place. Folding this into configd is tracked as a kernel-track
 * item (needs EVFILT_MACHPORT). See [[configd-port-state]].
 *
 * Job: translate kernel link-state changes into the SCDynamicStore so
 * consumers react to "the NIC's link came up" without polling. On
 * FreeBSD the signal is the PF_ROUTE socket's RTM_IFINFO message,
 * whose if_data.ifi_link_state carries LINK_STATE_UP/DOWN/UNKNOWN —
 * the FreeBSD analog of Darwin's KEV_DL_LINK_ON kernel event.
 *
 * For each interface it publishes Apple's canonical key
 *   State:/Network/Interface/<ifname>/Link = { Active : <bool> }
 * exactly as the real KernelEventMonitor does, so ipconfigd (and any
 * future IPMonitor-style consumer) watches one well-known key.
 *
 * This replaces ipconfigd's removed hwregd attach-subscription as the
 * "start DHCP now" trigger (PR #167); hwregd is no longer in the boot
 * path.
 */
#include <sys/types.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/route.h>

#include <SystemConfiguration/SCDynamicStore.h>
#include <CoreFoundation/CoreFoundation.h>

#include <errno.h>
#include <ifaddrs.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "KernelEventMonitor ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/* CFStringCreateWithCString wrapper — avoids -fconstant-cfstrings. */
static CFStringRef
mkstr(const char *s)
{
	return (CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8));
}

/*
 * Map a (link_state, if_flags) pair to Apple's "Active" boolean.
 * LINK_STATE_UP is an unambiguous yes. Many virtual / older NICs
 * report LINK_STATE_UNKNOWN (no media-status capability); for those we
 * fall back to IFF_UP, matching how SystemConfiguration treats
 * link-status-less interfaces as active once they are administratively
 * up. LINK_STATE_DOWN is a no regardless of flags.
 */
static int
link_active(int link_state, int if_flags)
{
	if (link_state == LINK_STATE_UP)
		return (1);
	if (link_state == LINK_STATE_DOWN)
		return (0);
	/* LINK_STATE_UNKNOWN */
	return ((if_flags & IFF_UP) != 0);
}

/*
 * Publish State:/Network/Interface/<ifname>/Link = { Active : bool }.
 * Loopback is skipped — it never carries a DHCP service and only adds
 * churn. Returns 0 on success.
 */
static int
publish_link(SCDynamicStoreRef store, const char *ifname, int active)
{
	CFMutableDictionaryRef dict;
	CFStringRef key, k_active;
	Boolean ok;

	if (strcmp(ifname, "lo0") == 0)
		return (0);

	dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (dict == NULL)
		return (-1);
	k_active = mkstr("Active");
	if (k_active != NULL) {
		CFDictionarySetValue(dict, k_active,
		    active ? kCFBooleanTrue : kCFBooleanFalse);
		CFRelease(k_active);
	}

	key = CFStringCreateWithFormat(NULL, NULL,
	    CFSTR("State:/Network/Interface/%s/Link"), ifname);
	if (key == NULL) {
		CFRelease(dict);
		return (-1);
	}
	ok = SCDynamicStoreSetValue(store, key, dict);
	CFRelease(key);
	CFRelease(dict);

	if (!ok) {
		xlog("SCDynamicStoreSetValue(%s/Link) failed: %s",
		    ifname, SCErrorString(SCError()));
		return (-1);
	}
	/* KEM-LINK-OK gates in boot-test.sh: proves a link-state change
	 * reached the store, the signal ipconfigd's watch depends on. */
	xlog("KEM-LINK-OK: %s Active=%d", ifname, active);
	return (0);
}

/*
 * Publish the current link state of every interface at startup. The
 * AF_LINK ifaddr's ifa_data is a `struct if_data *` on FreeBSD, which
 * carries ifi_link_state — so getifaddrs gives both flags and link
 * state in one pass. This is the safety net for a transition that
 * happened before we (or a watcher) started: the live state is
 * republished so a consumer that subscribes late still sees it.
 */
static void
publish_all(SCDynamicStoreRef store)
{
	struct ifaddrs *ifa, *p;

	if (getifaddrs(&ifa) != 0) {
		xlog("getifaddrs failed: %s", strerror(errno));
		return;
	}
	for (p = ifa; p != NULL; p = p->ifa_next) {
		const struct if_data *ifd;
		int active;

		if (p->ifa_addr == NULL ||
		    p->ifa_addr->sa_family != AF_LINK ||
		    p->ifa_data == NULL)
			continue;
		ifd = (const struct if_data *)p->ifa_data;
		active = link_active(ifd->ifi_link_state, (int)p->ifa_flags);
		(void)publish_link(store, p->ifa_name, active);
	}
	freeifaddrs(ifa);
}

/*
 * Open the configd session, retrying until configd is up — KEM and
 * configd are both RunAtLoad with no ordering guarantee, so the first
 * SCDynamicStoreCreate can race configd's bootstrap check-in.
 */
static SCDynamicStoreRef
open_store_blocking(void)
{
	CFStringRef name = mkstr("KernelEventMonitor");
	SCDynamicStoreRef store = NULL;
	int tries;

	for (tries = 0; tries < 120; tries++) {
		store = SCDynamicStoreCreate(NULL, name, NULL, NULL);
		if (store != NULL)
			break;
		(void)sleep(1);
	}
	if (name != NULL)
		CFRelease(name);
	return (store);
}

int
main(int argc, char **argv)
{
	SCDynamicStoreRef store;
	int rs;

	(void)argc;
	(void)argv;

	xlog("KernelEventMonitor starting (PF_ROUTE link-state -> "
	    "State:/Network/Interface/<if>/Link)");

	store = open_store_blocking();
	if (store == NULL) {
		xlog("KEM-FAIL: no configd session after 120s");
		return (1);
	}

	rs = socket(PF_ROUTE, SOCK_RAW, 0);
	if (rs < 0) {
		xlog("KEM-FAIL: socket(PF_ROUTE): %s", strerror(errno));
		CFRelease(store);
		return (1);
	}

	/* Seed the store with current link state before blocking on the
	 * route socket, so a watcher that is already up sees the snapshot. */
	publish_all(store);

	for (;;) {
		char buf[2048];
		struct rt_msghdr *rtm;
		struct if_msghdr *ifm;
		char ifname[IFNAMSIZ];
		ssize_t n;
		int active;

		n = read(rs, buf, sizeof(buf));
		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			xlog("KEM-FAIL: read(PF_ROUTE): %s",
			    n < 0 ? strerror(errno) : "EOF");
			break;
		}
		if ((size_t)n < sizeof(struct rt_msghdr))
			continue;
		rtm = (struct rt_msghdr *)(void *)buf;
		if (rtm->rtm_version != RTM_VERSION)
			continue;
		/* RTM_IFINFO carries link-state + flags changes. */
		if (rtm->rtm_type != RTM_IFINFO)
			continue;
		ifm = (struct if_msghdr *)(void *)buf;
		if (if_indextoname(ifm->ifm_index, ifname) == NULL)
			continue;

		active = link_active(ifm->ifm_data.ifi_link_state,
		    ifm->ifm_flags);
		(void)publish_link(store, ifname, active);
	}

	(void)close(rs);
	CFRelease(store);
	return (1);	/* loop only exits on a fatal route-socket error */
}
