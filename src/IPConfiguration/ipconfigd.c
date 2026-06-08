/*
 * ipconfigd — freebsd-launchd-mach IPConfiguration daemon.
 *
 * The Mach-IPC track port of Apple's IPConfiguration
 * (apple-oss-distributions/bootp / IPConfiguration.bproj). Apple
 * runs it as a configd plugin; we run standalone because this
 * repo's configd has no plugin loader (see [[configd-port-state]]
 * memory). Plan inventory + amputation list at
 * pkgdemon.github.io/freebsd-ipconfiguration-plan.html (note: that
 * doc targets the sibling AF_UNIX / GNUstep-DO repo; here we keep
 * Apple's Mach IPC + MIG, same as configd / hwregd).
 *
 * iter 1 — daemon skeleton. main + signal handling + getifaddrs
 * interface enumeration + bootstrap_check_in for
 * com.apple.IPConfiguration + a sleep loop that holds the service
 * port until SIGTERM. Marker IPCFG-BOOT-OK is emitted by the
 * separate `ipconfigtest` client (bootstrap_look_up against this
 * service) — same pattern hwregd / configd iter 1 use.
 *
 * iter 2 — one-shot DHCPv4 DISCOVER/OFFER probe (dhcp_discover.c).
 *
 * iter 3 — full RFC 2131 INIT → BOUND. dhcp_discover.c runs the
 * SELECTING → REQUESTING → BOUND state machine; apply_lease.c then
 * installs the result (SIOCAIFADDR, default route, /etc/resolv.conf).
 *
 * iter 4 — SCDynamicStore publish on BOUND + RFC 2131 §4.4.5
 * RENEWING/REBINDING lease loop. sc_publish.c builds the
 * State:/Network/Service/<UUID>/IPv4 dictionary; lease_loop.c
 * sleeps until T1, sends a broadcast RENEWING REQUEST, etc.
 *
 * iter 5a — raw mach_msg MIG demux + worker thread.
 * mach_service.c spawns a pthread that bootstrap_check_in's the
 * service port and runs _ipconfig_server() (MIG demux for
 * ipconfig.defs) on each request. The worker reads live state via
 * bound_state.{c,h}; the main thread (DHCP + lease loop) writes it
 * on BOUND. iter 5a vendors 2 read-only routines (if_count,
 * if_addr); the full ipconfig.defs surface grows in iter 6+.
 *
 * link-state DHCP trigger — react to link-up via SCDynamicStore.
 * sc_link_watch.c watches State:/Network/Interface/<if>/Link, which
 * the standalone KernelEventMonitor daemon publishes from PF_ROUTE
 * link-state changes; when an interface goes Active the watch invokes
 * on_link_event(), which admin-ups it (link down) or runs DHCP (link up) on it. This is the Apple-shaped
 * trigger (KernelEventMonitor -> SCDynamicStore -> IPConfiguration);
 * it replaced the earlier hwregd attach subscription (removed with
 * hwregd in PR #167). At startup we bring the candidate NIC IFF_UP so
 * its link negotiates, then let the watch fire DHCP — fixing the
 * stock-kernel case where a real NIC's link comes up a beat after a
 * one-shot startup scan would have given up.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <arpa/inet.h>		/* inet_ntop for the BOUND log line */

#include <errno.h>
#include <ifaddrs.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "apply_lease.h"
#include "apply_lease_v6.h"
#include "bound_state.h"
#include "dhcp_discover.h"
#include "lease_loop.h"
#include "mach_service.h"
#include "ra_listen.h"
#include "sc_link_watch.h"
#include "sc_publish.h"

/*
 * Global (non-static): dhcp_discover.c's recv loop polls this
 * between BPF reads so SIGTERM/SIGHUP during the multi-second
 * retransmit ladder short-circuits the wait. Declared in
 * dhcp_discover.h.
 */
volatile sig_atomic_t got_term;

/*
 * Serializes DHCP attempts driven by the link-watch callback. The
 * watch fires on a libdispatch worker thread and may fire more than
 * once (initial scan + an event, or several interfaces); g_dhcp_started
 * ensures only one run is in flight. It stays set while a successful
 * run blocks in lease_loop_run (we are bound), and is cleared if a run
 * returns (DHCP failed) so a later link event can retry.
 */
static pthread_mutex_t	g_dhcp_lock = PTHREAD_MUTEX_INITIALIZER;
static int		g_dhcp_started;

static void
on_signal(int sig)
{
	got_term = sig;
}

/*
 * Bring an interface administratively up (IFF_UP) so its link
 * negotiates — without touching addresses. ipconfigd does this at
 * startup for the candidate NIC; the resulting link-state change is
 * what KernelEventMonitor reports and our watch turns into a DHCP run.
 */
static int
iface_bring_up(const char *ifname)
{
	struct ifreq ifr;
	int s, rc = -1;

	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		return (-1);
	(void)memset(&ifr, 0, sizeof(ifr));
	(void)strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
		if ((ifr.ifr_flags & IFF_UP) != 0) {
			rc = 0;
		} else {
			ifr.ifr_flags |= IFF_UP;
			if (ioctl(s, SIOCSIFFLAGS, &ifr) == 0)
				rc = 0;
		}
	}
	(void)close(s);
	return (rc);
}

static void
xlog(const char *fmt, ...)
{
	struct timespec ts;
	struct tm tm;
	char tbuf[32];
	va_list ap;

	(void)clock_gettime(CLOCK_REALTIME, &ts);
	(void)gmtime_r(&ts.tv_sec, &tm);
	(void)strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", &tm);
	(void)fprintf(stderr, "ipconfigd %s ", tbuf);

	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/*
 * Walk getifaddrs(3) and log each interface's BSD name + family.
 * iter 1's "real" content: the same enumeration later iters use to
 * pick which interfaces to DHCP on. The AF_LINK pass also surfaces
 * the MAC address — useful when DHCP needs it for the chaddr.
 */
static int
enumerate_interfaces(void)
{
	struct ifaddrs *ifa, *p;
	int total = 0, with_link = 0;

	if (getifaddrs(&ifa) != 0) {
		xlog("getifaddrs failed: %s", strerror(errno));
		return (-1);
	}
	for (p = ifa; p != NULL; p = p->ifa_next) {
		const char *fam = "?";

		if (p->ifa_addr == NULL)
			continue;
		switch (p->ifa_addr->sa_family) {
		case AF_INET:	fam = "AF_INET"; break;
		case AF_INET6:	fam = "AF_INET6"; break;
		case AF_LINK:	fam = "AF_LINK"; with_link++; break;
		default:	continue;
		}
		xlog("iface: %s family=%s flags=0x%x", p->ifa_name, fam,
		    p->ifa_flags);
		total++;
	}
	freeifaddrs(ifa);
	xlog("interface scan: %d records (%d AF_LINK)", total, with_link);
	return (total);
}

/*
 * Read IPCONFIGD_FAST_LEASE from the environment. The env var caps
 * the effective lease time used for T1 / T2 timer math (the value
 * published to configd is unaffected). Accepts integers in [4, 86400]
 * seconds; outside that range, or unset, the cap is disabled (return
 * 0). Floor of 4 because the iter-3 retransmit ladder is 4s.
 */
static uint32_t
read_lease_cap_env(void)
{
	const char *s = getenv("IPCONFIGD_FAST_LEASE");
	char *end;
	long v;

	if (s == NULL || *s == '\0')
		return (0);
	v = strtol(s, &end, 10);
	if (end == s || *end != '\0' || v < 4 || v > 86400) {
		xlog("IPCONFIGD_FAST_LEASE='%s' ignored (need integer "
		    "in [4, 86400])", s);
		return (0);
	}
	xlog("IPCONFIGD_FAST_LEASE=%ld — capping lease for renewal "
	    "timer math (server lease still authoritative)", v);
	return ((uint32_t)v);
}

/*
 * dhcp_run_on_interface — full DHCPv4 INIT → BOUND → publish → RA →
 * lease loop on `ifname`. Driven by the link-watch callback
 * (on_link_event) when KernelEventMonitor reports the link Active.
 *
 * Blocks in lease_loop_run until SIGTERM; never returns from a fully
 * successful path. On any failure the function returns cleanly and
 * on_link_event re-arms so a later link-up event can retry.
 */
static void
dhcp_run_on_interface(const char *ifname, uint32_t lease_cap_secs)
{
	struct dhcp_lease lease;
	struct sc_publish *pub;
	char a[INET_ADDRSTRLEN], m[INET_ADDRSTRLEN];
	char r[INET_ADDRSTRLEN], s[INET_ADDRSTRLEN];
	struct ra_info ra;
	int rar;

	xlog("selected interface for DHCPv4: %s", ifname);
	if (dhcp_lease_acquire(ifname, &lease) != 0) {
		/* dhcp_lease_acquire logged IPCFG-BOUND-FAIL on its own line */
		return;
	}

	if (apply_lease(ifname, &lease) != 0) {
		xlog("apply_lease(%s) failed", ifname);
		xlog("IPCFG-BOUND-FAIL");
		return;
	}

	(void)inet_ntop(AF_INET, &lease.addr, a, sizeof(a));
	(void)inet_ntop(AF_INET, &lease.netmask, m, sizeof(m));
	(void)inet_ntop(AF_INET, &lease.router, r, sizeof(r));
	(void)inet_ntop(AF_INET, &lease.server, s, sizeof(s));
	xlog("bound: iface=%s addr=%s netmask=%s router=%s "
	    "server=%s lease=%us",
	    ifname, a, m, r, s, (unsigned)lease.lease_time);
	/*
	 * Make the lease visible to the Mach service worker before the
	 * BOUND marker fires — so any client racing IPCFG-RPC-OK against
	 * IPCFG-BOUND-OK sees the address it expects.
	 */
	bound_state_set(ifname, &lease);
	xlog("IPCFG-BOUND-OK");

	/*
	 * Publish to configd. Failure is non-fatal: the daemon stays up,
	 * the lease is already applied at the kernel level, but
	 * observers watching State:/Network/Service/... won't see this
	 * binding. CI marker IPCFG-STORE-FAIL flags it.
	 */
	pub = sc_publish_open("ipconfigd");
	if (pub == NULL) {
		xlog("IPCFG-STORE-FAIL: no configd session");
		return;
	}
	if (sc_publish_ipv4(pub, ifname, &lease) != 0) {
		xlog("IPCFG-STORE-FAIL: set State:/.../IPv4");
		sc_publish_close(pub);
		return;
	}
	xlog("IPCFG-STORE-OK");

	/*
	 * Issue #88: publish State:/Network/Service/<UUID>/DHCP carrying
	 * InterfaceName + LeaseStartTime, and Option_12 (host name) when
	 * the lease supplied it. SLIRP doesn't ship Option_12 so the
	 * marker proves the key/dict shape is correct; hostnamed iter 3
	 * is the first consumer that reads it. Failure is non-fatal —
	 * the IPv4 publish already succeeded.
	 */
	if (sc_publish_dhcp(pub, ifname, &lease) != 0) {
		xlog("IPCFG-DHCP-FAIL: set State:/.../DHCP");
	} else {
		xlog("IPCFG-DHCP-OK: published /DHCP "
		    "(Option_12 %s)",
		    lease.host_name_len > 0
		        ? lease.host_name : "absent");
	}

	/*
	 * iter 7a: solicit + listen for one RA, derive a SLAAC address,
	 * install it + the v6 default route, and publish
	 * State:/.../IPv6. 15s budget — QEMU SLIRP answers within ms,
	 * so a miss means RA isn't configured; we log IPCFG-RA-MISS and
	 * continue with IPv4 only.
	 *
	 * Round-1 CI showed em0 has ND6_IFF_IFDISABLED set (this image's
	 * net.inet6.ip6.auto_linklocal is 0, so the kernel never
	 * auto-added a link-local). bring_v6_up clears IFDISABLED and
	 * installs fe80::EUI-64 so the kernel can source-select the
	 * link-local-scoped RS.
	 */
	(void)bring_v6_up(ifname);
	rar = ra_acquire(ifname, 15000, &ra);
	if (rar == 0) {
		struct in6_addr v6;

		if (apply_ra_lease(ifname, &ra, &v6) == 0) {
			(void)sc_publish_ipv6(pub, ifname, &v6,
			    ra.prefix_len, &ra.router_lladdr);
			xlog("IPCFG-RA-OK");
		} else {
			xlog("IPCFG-RA-MISS: apply_ra_lease failed");
		}
	} else if (rar == 1) {
		xlog("IPCFG-RA-MISS: no RA in 15s "
		    "(SLIRP may not advertise IPv6)");
	} else {
		xlog("IPCFG-RA-MISS: ra_acquire fatal");
	}

	(void)lease_loop_run(ifname, &lease, pub, lease_cap_secs);
	sc_publish_close(pub);
}

/*
 * link-watch callback — invoked on a libdispatch worker thread for every
 * change to an interface's Link entity (State:/Network/Interface/<if>/Link),
 * published by KernelEventMonitor from PF_ROUTE. `active` reflects the current
 * link state. This is the Apple-shaped trigger that replaced the hwregd attach
 * subscription.
 *
 * Two cases:
 *   - link DOWN (active == 0): the interface is present but its link has not
 *     come up. Bring it administratively up so its link can negotiate. This is
 *     the critical path for a NIC that ARRIVES after startup — e.g. an
 *     auto-loaded driver kext (#219): ipconfigd's startup scan never saw it, so
 *     nothing else admin-ups it, and without IFF_UP the link stays down forever
 *     and it is never DHCP'd. iface_bring_up is idempotent (no-op if already
 *     up), so re-fires for an interface we already brought up are harmless. The
 *     resulting link-up generates a fresh Active:true event that lands us in the
 *     DHCP case below.
 *   - link UP (active != 0): run DHCP on the just-linked interface, guarding
 *     against concurrent / duplicate fires (skip if a run is in flight or we are
 *     already bound). Single-NIC focus is unchanged; multi-NIC fan-out is a
 *     later iter. dhcp_run_on_interface blocks in lease_loop_run on this worker
 *     thread once bound — fine, libdispatch services other work on other threads.
 */
static void
on_link_event(const char *ifname, int active, uint32_t lease_cap_secs)
{
	char already[IFNAMSIZ] = "";
	int i;

	/* loopback never carries a DHCP service; the watcher filters lo0 too. */
	if (strncmp(ifname, "lo", 2) == 0)
		return;

	if (!active) {
		/* Present but link down — admin-up so the link can negotiate
		 * (the late-arriving-NIC onboarding path, #219).
		 *
		 * Recovery for the lost-wakeup race (#250): KEM publishes Active:0
		 * then Active:1 in quick succession for a late NIC. configd sends a
		 * watcher wakeup only on the empty->non-empty edge, so if the
		 * Active:1 _configset is demuxed by configd's single serve thread
		 * between our wakeup and our notifychanges drain, no second wakeup
		 * is sent and our callout reads the store one instant too early
		 * (Active:0). The Active:1 value then sits in the store with no
		 * pending notification and we would wait forever. The store *value*
		 * is authoritative even when the *wakeup* is not, so poll it a few
		 * times after admin-up and recover by re-driving the active path.
		 * This also covers a configd full-queue silent send-drop and any
		 * future transport change — it does not depend on the wakeup at all. */
		if (iface_bring_up(ifname) != 0) {
			xlog("link-seen(%s) — could not bring up: %s", ifname,
			    strerror(errno));
			return;
		}
		xlog("link-seen(%s) — brought admin-up; polling link state",
		    ifname);
		for (i = 0; i < 6; i++) {
			struct timespec ts = { .tv_sec = 0,
			    .tv_nsec = 500 * 1000 * 1000 };	/* 500ms */

			(void)nanosleep(&ts, NULL);
			if (link_active_in_store(ifname)) {
				xlog("link-seen(%s) — store shows Active after "
				    "admin-up; recovering missed wakeup, DHCP",
				    ifname);
				on_link_event(ifname, 1, lease_cap_secs);
				return;
			}
		}
		xlog("link-seen(%s) — still down after admin-up poll; "
		    "awaiting Active wakeup", ifname);
		return;
	}

	pthread_mutex_lock(&g_dhcp_lock);
	if (g_dhcp_started || bound_state_any(already, sizeof(already))) {
		pthread_mutex_unlock(&g_dhcp_lock);
		return;
	}
	g_dhcp_started = 1;
	pthread_mutex_unlock(&g_dhcp_lock);

	xlog("link-active(%s) — running DHCP", ifname);
	dhcp_run_on_interface(ifname, lease_cap_secs);

	/*
	 * Only reached if DHCP failed (a fully successful run blocks in
	 * lease_loop_run and never returns). Re-arm so a later link event
	 * can retry.
	 */
	pthread_mutex_lock(&g_dhcp_lock);
	g_dhcp_started = 0;
	pthread_mutex_unlock(&g_dhcp_lock);
}

int
main(int argc, char **argv)
{
	struct sigaction sa;
	uint32_t lease_cap_secs;
	char ifname[IFNAMSIZ] = "";

	(void)argc;
	(void)argv;

	xlog("ipconfigd starting (MIG service + DHCPv4 + RFC 5227 ARP + "
	    "IPv6 RA/SLAAC + link-state DHCP trigger)");

	lease_cap_secs = read_lease_cap_env();

	/*
	 * sa_flags = 0 — system calls are NOT auto-restarted on
	 * signal. That's deliberate: a SIGTERM/SIGHUP during
	 * dhcp_discover.c's BPF read returns EINTR, the read loop
	 * checks got_term and bails out instead of resuming the
	 * (potentially-multi-second) wait. iter-2 had no such hook.
	 */
	(void)memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	(void)sigaction(SIGTERM, &sa, NULL);
	(void)sigaction(SIGINT, &sa, NULL);
	(void)sigaction(SIGHUP, &sa, NULL);

	if (enumerate_interfaces() < 0) {
		xlog("interface enumeration failed; continuing anyway");
		/* not fatal — the daemon still exposes its Mach service */
	}

	/*
	 * iter 5a: the Mach service thread now owns the receive right
	 * for com.apple.IPConfiguration (bootstrap_check_in lives in
	 * mach_service.c). It is spawned up-front so the service is
	 * reachable while DHCP is still in flight — RPC routines
	 * return ipconfig_status_no_server_e until the BOUND lease
	 * lands in bound_state. iter-1 IPCFG-BOOT (ipconfigtest's
	 * bootstrap_look_up) is now answered by launchd's broker, not
	 * a direct send to our checked-in port; the Mach plumbing is
	 * identical from the client side.
	 */
	bound_state_init();
	if (mach_service_start() != 0)
		xlog("mach_service_start failed; RPC off");

	/*
	 * Apple-shaped DHCP trigger. Rather than DHCP unconditionally at
	 * startup (which races a real NIC whose link negotiates a beat
	 * after we scan — the failure mode that left a stock-kernel box
	 * without a lease once hwregd's attach event was removed), we:
	 *
	 *   1. bring the candidate Ethernet administratively up so its
	 *      link starts negotiating, then
	 *   2. watch State:/Network/Interface/<if>/Link and run DHCP only
	 *      when KernelEventMonitor reports the link Active.
	 *
	 * The watch's initial scan + KernelEventMonitor's startup snapshot
	 * make this fire immediately when the link is already up (CI /
	 * SLIRP), and exactly once when it comes up later (real hardware).
	 * DHCP itself (INIT → BOUND, apply_lease, the SCDynamicStore
	 * publish, and the RENEWING/REBINDING lease loop) is unchanged —
	 * see dhcp_run_on_interface; markers IPCFG-BOUND-OK / -STORE-OK /
	 * -DHCP-OK fire from there. A failed run does NOT exit ipconfigd;
	 * the Mach service stays registered and the watch re-arms.
	 */
	if (dhcp_pick_interface(ifname, sizeof(ifname)) == 0) {
		if (iface_bring_up(ifname) == 0)
			xlog("brought %s up; awaiting link-active to DHCP",
			    ifname);
		else
			xlog("could not bring %s up: %s — relying on link "
			    "watch", ifname, strerror(errno));
	} else {
		xlog("no Ethernet at startup; will DHCP when one links up");
	}

	if (sc_link_watch_start(on_link_event, lease_cap_secs) != 0)
		xlog("IPCFG-BOUND-FAIL: link watch unavailable — DHCP will "
		    "not be triggered");

	/*
	 * Hold the daemon alive. DHCP runs on the link-watch's worker
	 * thread; the main thread just waits for SIGTERM. A bound lease's
	 * renewal loop also runs on that worker thread.
	 */
	while (!got_term) {
		(void)sleep(60);
	}

	xlog("ipconfigd exiting on signal %d", (int)got_term);
	mach_service_join();
	return (0);
}
