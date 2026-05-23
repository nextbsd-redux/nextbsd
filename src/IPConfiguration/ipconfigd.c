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
 * After bootstrap_check_in, ipconfigd picks the first non-loopback
 * Ethernet interface, brings it up, opens BPF, sends DHCPDISCOVER
 * and waits for a DHCPOFFER. Logs IPCFG-DISCOVER-OK/FAIL to stderr
 * (run.sh cats the stderr file post-boot so the marker reaches the
 * boot-test console). Full INIT → BOUND with SIOCSIFADDR + the
 * default route is iter 3+.
 */
#include <sys/types.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <errno.h>
#include <ifaddrs.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "dhcp_discover.h"

#define IPCFG_SERVICE_NAME	"com.apple.IPConfiguration"

static volatile sig_atomic_t got_term;

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

int
main(int argc, char **argv)
{
	struct sigaction sa;
	mach_port_t service = MACH_PORT_NULL;
	kern_return_t kr;

	(void)argc;
	(void)argv;

	xlog("ipconfigd starting (iter 1 skeleton — no DHCP yet)");

	(void)memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	(void)sigaction(SIGTERM, &sa, NULL);
	(void)sigaction(SIGINT, &sa, NULL);

	if (enumerate_interfaces() < 0) {
		xlog("interface enumeration failed; continuing anyway");
		/* not fatal — the daemon still exposes its Mach service */
	}

	/*
	 * launchd handed us the receive right for the
	 * com.apple.IPConfiguration MachServices entry via the
	 * bootstrap port. bootstrap_check_in claims it. Without
	 * RunAtLoad-then-check-in the service would not be reachable
	 * from clients.
	 */
	kr = bootstrap_check_in(bootstrap_port, IPCFG_SERVICE_NAME,
	    &service);
	if (kr != KERN_SUCCESS) {
		xlog("bootstrap_check_in('%s') failed: 0x%x — "
		    "ipconfigtest will not see the service",
		    IPCFG_SERVICE_NAME, (unsigned)kr);
		/* don't exit; let launchd respawn surface the issue */
	} else {
		xlog("bootstrap_check_in('%s') ok: service port=0x%x",
		    IPCFG_SERVICE_NAME, (unsigned)service);
	}

	/*
	 * iter 2: one-shot DHCPv4 DISCOVER/OFFER probe on the first
	 * non-loopback Ethernet interface (em0 in CI). Logs
	 * IPCFG-DISCOVER-OK/FAIL via dhcp_discover_run's xlog. We do
	 * not exit on failure — the daemon keeps the Mach service
	 * registered either way; the marker is the test signal.
	 * iter 3+ replaces this one-shot with the full INIT → BOUND
	 * state machine.
	 */
	{
		char ifname[IFNAMSIZ] = "";

		if (dhcp_pick_interface(ifname, sizeof(ifname)) == 0) {
			xlog("selected interface for DHCPv4 probe: %s",
			    ifname);
			(void)dhcp_discover_run(ifname);
		} else {
			xlog("IPCFG-DISCOVER-FAIL: no Ethernet interface "
			    "found");
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
	return (0);
}
