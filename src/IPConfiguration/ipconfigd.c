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

int
main(int argc, char **argv)
{
	struct sigaction sa;
	uint32_t lease_cap_secs;

	(void)argc;
	(void)argv;

	xlog("ipconfigd starting (iter 7a — MIG service + DHCPv4 + "
	    "RFC 5227 ARP + IPv6 RA/SLAAC)");

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
	 * registered (iter-1 IPCFG-BOOT still passes), and a future
	 * iter's retry logic will pick up.
	 */
	{
		char ifname[IFNAMSIZ] = "";

		if (dhcp_pick_interface(ifname, sizeof(ifname)) != 0) {
			xlog("no Ethernet interface found for DHCPv4");
			xlog("IPCFG-BOUND-FAIL");
		} else {
			struct dhcp_lease lease;

			xlog("selected interface for DHCPv4: %s", ifname);
			if (dhcp_lease_acquire(ifname, &lease) == 0) {
				char a[INET_ADDRSTRLEN], m[INET_ADDRSTRLEN];
				char r[INET_ADDRSTRLEN], s[INET_ADDRSTRLEN];

				if (apply_lease(ifname, &lease) != 0) {
					xlog("apply_lease(%s) failed",
					    ifname);
					xlog("IPCFG-BOUND-FAIL");
				} else {
					struct sc_publish *pub;

					(void)inet_ntop(AF_INET,
					    &lease.addr, a, sizeof(a));
					(void)inet_ntop(AF_INET,
					    &lease.netmask, m, sizeof(m));
					(void)inet_ntop(AF_INET,
					    &lease.router, r, sizeof(r));
					(void)inet_ntop(AF_INET,
					    &lease.server, s, sizeof(s));
					xlog("bound: iface=%s addr=%s "
					    "netmask=%s router=%s "
					    "server=%s lease=%us",
					    ifname, a, m, r, s,
					    (unsigned)lease.lease_time);
					/*
					 * Make the lease visible to the
					 * Mach service worker before the
					 * BOUND marker fires — so any
					 * client racing IPCFG-RPC-OK
					 * against IPCFG-BOUND-OK sees the
					 * address it expects.
					 */
					bound_state_set(ifname, &lease);
					xlog("IPCFG-BOUND-OK");

					/*
					 * Publish to configd. Failure is
					 * non-fatal: the daemon stays up,
					 * the lease is already applied at
					 * the kernel level, but observers
					 * watching State:/Network/Service/.
					 * won't see this binding. CI marker
					 * IPCFG-STORE-FAIL flags it.
					 */
					pub = sc_publish_open("ipconfigd");
					if (pub == NULL) {
						xlog("IPCFG-STORE-FAIL: "
						    "no configd session");
					} else if (sc_publish_ipv4(pub,
					    ifname, &lease) != 0) {
						xlog("IPCFG-STORE-FAIL: "
						    "set State:/.../IPv4");
					} else {
						struct ra_info ra;
						int rar;

						xlog("IPCFG-STORE-OK");

						/*
						 * iter 7a: solicit + listen for
						 * one RA, derive a SLAAC address,
						 * install it + the v6 default
						 * route, and publish State:/.../
						 * IPv6. 15s budget — QEMU SLIRP
						 * answers within ms, so a miss
						 * means RA isn't configured;
						 * we log IPCFG-RA-MISS and
						 * continue with IPv4 only.
						 */
						rar = ra_acquire(ifname,
						    15000, &ra);
						if (rar == 0) {
							struct in6_addr v6;

							if (apply_ra_lease(
							    ifname, &ra,
							    &v6) == 0) {
								(void)
								sc_publish_ipv6(
								    pub, ifname,
								    &v6,
								    ra.prefix_len,
								    &ra.router_lladdr);
								xlog("IPCFG-RA-OK");
							} else {
								xlog("IPCFG-RA-MISS: "
								    "apply_ra_lease failed");
							}
						} else if (rar == 1) {
							xlog("IPCFG-RA-MISS: "
							    "no RA in 15s "
							    "(SLIRP may not "
							    "advertise IPv6)");
						} else {
							xlog("IPCFG-RA-MISS: "
							    "ra_acquire fatal");
						}

						(void)lease_loop_run(ifname,
						    &lease, pub,
						    lease_cap_secs);
					}
					if (pub != NULL)
						sc_publish_close(pub);
				}
			}
			/* failure case: dhcp_lease_acquire already logged
			 * the marker on its own line */
		}
	}

	/*
	 * Hold the daemon alive. iter 1/2 have no MIG demux; later
	 * iters replace this loop with a mach_msg(MACH_RCV_MSG ...)
	 * receive over the service port (the configd / hwregd shape).
	 */
	while (!got_term) {
		(void)sleep(60);
	}

	xlog("ipconfigd exiting on signal %d", (int)got_term);
	mach_service_join();
	return (0);
}
