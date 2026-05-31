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

/*
 * The configd session. Held in a file-scope global so a publish that
 * fails (configd restarted -> the session's server port is dead, e.g.
 * MACH_SEND_INVALID_DEST) can drop and reopen it, then re-sync the full
 * link-state snapshot into the fresh (empty) store.
 */
static SCDynamicStoreRef g_store;

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
 * Open the configd session into g_store, retrying until configd is up —
 * KEM and configd are both RunAtLoad with no ordering guarantee, so the
 * first SCDynamicStoreCreate can race configd's bootstrap check-in, and
 * a mid-run reopen can race configd's respawn. Returns 0 on success.
 */
static int
store_open(void)
{
	CFStringRef name = mkstr("KernelEventMonitor");
	int tries;

	for (tries = 0; tries < 120; tries++) {
		g_store = SCDynamicStoreCreate(NULL, name, NULL, NULL);
		if (g_store != NULL)
			break;
		(void)sleep(1);
	}
	if (name != NULL)
		CFRelease(name);
	return (g_store != NULL ? 0 : -1);
}

static void
store_drop(void)
{
	if (g_store != NULL) {
		CFRelease(g_store);
		g_store = NULL;
	}
}

/*
 * Raw publish of State:/Network/Interface/<ifname>/Link = { Active:bool }
 * into the current g_store. No reopen logic — that lives in
 * publish_link. Loopback is skipped (it never carries a DHCP service).
 * Returns 0 on success (including the skipped-loopback case), -1 if the
 * SetValue failed (session likely dead) or g_store is closed.
 */
static int
set_link_raw(const char *ifname, int active)
{
	CFMutableDictionaryRef dict;
	CFStringRef key, k_active;
	Boolean ok;

	if (strcmp(ifname, "lo0") == 0)
		return (0);
	if (g_store == NULL)
		return (-1);

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
	ok = SCDynamicStoreSetValue(g_store, key, dict);
	CFRelease(key);
	CFRelease(dict);

	if (!ok)
		return (-1);
	/* KEM-LINK-OK gates in boot-test.sh: proves a link-state change
	 * reached the store, the signal ipconfigd's watch depends on. */
	xlog("KEM-LINK-OK: %s Active=%d", ifname, active);
	return (0);
}

/*
 * Publish the current link state of every interface. The AF_LINK
 * ifaddr's ifa_data is a `struct if_data *` on FreeBSD, which carries
 * ifi_link_state — so getifaddrs gives both flags and link state in one
 * pass. Used at startup (safety net for a transition that happened
 * before a watcher subscribed) and to re-seed configd's store after a
 * reopen, since a restarted configd comes back with an empty store.
 */
static void
publish_all(void)
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
		(void)set_link_raw(p->ifa_name, active);
	}
	freeifaddrs(ifa);
}

/*
 * Publish one link-state change, healing a dead session. If the raw
 * SetValue fails (configd restarted -> MACH_SEND_INVALID_DEST), drop and
 * reopen the session, re-seed the whole snapshot into the fresh (empty)
 * store, and retry this event — so link updates keep flowing across a
 * configd restart instead of going silently stale.
 */
static void
publish_link(const char *ifname, int active)
{
	if (set_link_raw(ifname, active) == 0)
		return;		/* success, or loopback skip */

	xlog("publish(%s) failed: %s — reopening configd session",
	    ifname, SCErrorString(SCError()));
	store_drop();
	if (store_open() != 0) {
		xlog("KEM-FAIL: configd session could not be reopened");
		return;
	}
	publish_all();			/* re-seed the restarted, empty store */
	(void)set_link_raw(ifname, active);
}

int
main(int argc, char **argv)
{
	int rs;

	(void)argc;
	(void)argv;

	xlog("KernelEventMonitor starting (PF_ROUTE link-state -> "
	    "State:/Network/Interface/<if>/Link)");

	if (store_open() != 0) {
		xlog("KEM-FAIL: no configd session after 120s");
		return (1);
	}

	rs = socket(PF_ROUTE, SOCK_RAW, 0);
	if (rs < 0) {
		xlog("KEM-FAIL: socket(PF_ROUTE): %s", strerror(errno));
		store_drop();
		return (1);
	}

	/* Seed the store with current link state before blocking on the
	 * route socket, so a watcher that is already up sees the snapshot. */
	publish_all();

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
		publish_link(ifname, active);
	}

	(void)close(rs);
	store_drop();
	return (1);	/* loop only exits on a fatal route-socket error */
}
