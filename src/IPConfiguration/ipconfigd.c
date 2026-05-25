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
 * iter 9 — react to NIC arrival via hwregd subscription.
 * hwreg_subscribe.c bootstrap_look_up's org.freebsd.hwregd and
 * subscribes to its raw pub/sub stream; on a `+` attach event the
 * subscriber thread invokes on_attach_event(), which (if no NIC is
 * yet bound) re-runs dhcp_pick_interface and calls
 * dhcp_run_on_interface on the newly-visible NIC. Closes the "first
 * boot on a NIC whose driver hwregd autoloads ~60s into boot" gap
 * left by the iter "drain the deferred backlog" PR (#60). Marker
 * IPCFG-AUTOLOAD-SUB-OK fires when the subscription is established
 * (the autoload-then-DHCP end-to-end path isn't exercised in CI yet
 * because the CI kernel still has `device em` built in — a separate
 * iter that slims the kernel proves the full chain).
 */
#include <sys/types.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <arpa/inet.h>		/* inet_ntop for the BOUND log line */

#include <errno.h>
#include <ifaddrs.h>
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
#include "hwreg_subscribe.h"
#include "lease_loop.h"
#include "mach_service.h"
#include "ra_listen.h"
#include "sc_publish.h"

/*
 * Global (non-static): dhcp_discover.c's recv loop polls this
 * between BPF reads so SIGTERM/SIGHUP during the multi-second
 * retransmit ladder short-circuits the wait. Declared in
 * dhcp_discover.h.
 */
volatile sig_atomic_t got_term;

static void
on_signal(int sig)
{
	got_term = sig;
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
 * lease loop on `ifname`. Extracted from main() so both the startup
 * path and the hwregd subscriber callback (iter 9) can drive it.
 *
 * Blocks in lease_loop_run until SIGTERM; never returns from a fully
 * successful path. On any failure the function returns cleanly and
 * the caller decides what to do (typically: fall through to the
 * sleep loop and wait for the next hwregd attach).
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
 * iter 9 hwregd-subscriber callback — invoked from the subscriber
 * thread (hwreg_subscribe.c) when hwregd posts a `+` attach event.
 * If we already have a binding (the startup-path picked an interface
 * and is in lease_loop_run, or an earlier attach fired DHCP), skip:
 * the iter is single-NIC-focused, multi-NIC fan-out is a later
 * iter. Otherwise confirm the new device is an Ethernet (via
 * dhcp_pick_interface which already filters AF_LINK + IFT_ETHER +
 * !loopback — its "first match wins" rule means the just-attached
 * NIC will be picked once it's in getifaddrs) and run the full
 * DHCP+publish+lease loop on it.
 */
static void
on_attach_event(const char *attached_ifname, uint32_t lease_cap_secs)
{
	char picked[IFNAMSIZ] = "";
	char already[IFNAMSIZ] = "";

	if (bound_state_any(already, sizeof(already))) {
		xlog("attach(dev=%s): skipping — already bound on %s",
		    attached_ifname, already);
		return;
	}

	if (dhcp_pick_interface(picked, sizeof(picked)) != 0) {
		xlog("attach(dev=%s): no Ethernet visible yet via "
		    "getifaddrs — will retry on next event",
		    attached_ifname);
		return;
	}

	xlog("attach(dev=%s) — running DHCP on %s",
	    attached_ifname, picked);
	dhcp_run_on_interface(picked, lease_cap_secs);
}

int
main(int argc, char **argv)
{
	struct sigaction sa;
	uint32_t lease_cap_secs;
	char ifname[IFNAMSIZ] = "";

	(void)argc;
	(void)argv;

	xlog("ipconfigd starting (iter 9 — MIG service + DHCPv4 + "
	    "RFC 5227 ARP + IPv6 RA/SLAAC + hwregd attach subscriber)");

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
	 * iter 9: start the hwregd attach subscriber BEFORE the initial
	 * DHCP attempt so a NIC that hwregd autoloads while we're
	 * starting up still goes through on_attach_event. The
	 * subscription itself is unconditional (success on em0 → main
	 * thread enters lease_loop_run and never returns; subscriber
	 * thread idles but is harmless). Failure to subscribe is
	 * non-fatal — DHCP on the startup-path NIC still runs.
	 */
	(void)hwreg_subscribe_start(on_attach_event, lease_cap_secs);

	/*
	 * iter 3: full DHCPv4 INIT → BOUND on the first non-loopback
	 * Ethernet interface (em0 in CI). dhcp_lease_acquire runs the
	 * SELECTING → REQUESTING → BOUND state machine; apply_lease
	 * installs the result (SIOCAIFADDR, default route, DNS).
	 * Marker IPCFG-BOUND-OK / IPCFG-BOUND-FAIL — emitted by the
	 * helpers on their own log lines so the boot-test console
	 * captures the diagnostic before the marker fires expect.
	 *
	 * iter 4: on BOUND, publish State:/Network/Service/<UUID>/IPv4
	 * (+ optional /DNS) to configd via libSystemConfiguration —
	 * marker IPCFG-STORE-OK. Then enter the post-BOUND lease loop:
	 * sleep until T1, RENEWING, on fail sleep to T2, REBINDING.
	 *
	 * Failure does NOT exit ipconfigd — the Mach service stays
	 * registered (iter-1 IPCFG-BOOT still passes), and iter 9's
	 * hwregd subscriber will pick up if a NIC arrives later.
	 */
	if (dhcp_pick_interface(ifname, sizeof(ifname)) != 0) {
		xlog("no Ethernet interface found for DHCPv4 at startup "
		    "— waiting for hwregd attach event");
		xlog("IPCFG-BOUND-FAIL");
	} else {
		dhcp_run_on_interface(ifname, lease_cap_secs);
	}

	/*
	 * Hold the daemon alive after a failed startup DHCP (or after
	 * lease_loop_run unexpectedly returns). The hwregd subscriber
	 * thread is still listening and will run DHCP if a NIC arrives.
	 */
	while (!got_term) {
		(void)sleep(60);
	}

	xlog("ipconfigd exiting on signal %d", (int)got_term);
	mach_service_join();
	return (0);
}
