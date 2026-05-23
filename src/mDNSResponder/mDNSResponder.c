/*
 * mDNSResponder — freebsd-launchd-mach Bonjour / zeroconf daemon.
 *
 * iter 1: daemon skeleton. main + signal handling +
 * bootstrap_check_in for the com.apple.mDNSResponder Mach service +
 * a sleep loop that holds the service port until SIGTERM. No mDNS
 * engine yet — iter 2 vendors mDNSCore + mDNSPosix + mDNSShared and
 * wires the multicast listener + the DNS-SD client wire protocol.
 *
 * Same shape as ipconfigd / hwregd / configd iter 1: stand the
 * daemon up first, prove the Mach plumbing + launchd MachServices
 * routing + boot-test marker work, THEN import 4 MB of Apple
 * source and link it in. iter-1 marker MDNS-BOOT-OK is emitted by
 * the separate `mdnstest` client (bootstrap_look_up against the
 * service) — same pattern hwregd / configd / ipconfigd use.
 *
 * The Mach service receive right is held by this main thread via
 * mach_msg(MACH_RCV_MSG) inside a loop. iter 2 will replace the
 * raw loop with a MIG demux for dnssd.defs (Apple-canonical IDL
 * filename, mirrors bootp/ipconfig.defs shape).
 */
#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
	(void)fprintf(stderr, "mDNSResponder %s ", tbuf);

	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

int
main(int argc, char **argv)
{
	struct sigaction sa;
	mach_port_t svc = MACH_PORT_NULL;
	kern_return_t kr;

	(void)argc;
	(void)argv;

	xlog("starting (iter 1 — daemon skeleton, no mDNS engine yet)");

	(void)memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	(void)sigaction(SIGTERM, &sa, NULL);
	(void)sigaction(SIGINT, &sa, NULL);
	(void)sigaction(SIGHUP, &sa, NULL);

	/*
	 * Claim the Mach service. Same pattern hwregd / configd /
	 * ipconfigd use: bootstrap_check_in returns the receive right
	 * for the service port that launchd's plist-declared
	 * MachServices broker has set up. Once we hold the right,
	 * peer bootstrap_look_up calls route through launchd to our
	 * receive port — even before we start serving real RPC, the
	 * lookup succeeds, which is exactly what the iter-1 MDNS-BOOT
	 * marker proves.
	 */
	kr = bootstrap_check_in(bootstrap_port, "com.apple.mDNSResponder",
	    &svc);
	if (kr != KERN_SUCCESS || svc == MACH_PORT_NULL) {
		xlog("MDNS-BOOT-FAIL: bootstrap_check_in: 0x%x", (unsigned)kr);
		return (1);
	}
	xlog("MDNS-BOOT-OK: com.apple.mDNSResponder registered "
	    "(receive right=0x%x)", (unsigned)svc);

	/*
	 * iter-1 hold-alive loop. Spin on sleep(60) — SIGTERM/SIGHUP
	 * short-circuit. iter 2 replaces this with a mach_msg
	 * MACH_RCV_MSG loop calling the MIG-generated dnssd.defs
	 * server demux, exactly like ipconfigd's mach_service.c.
	 */
	while (!got_term)
		(void)sleep(60);

	xlog("exiting on signal %d", (int)got_term);
	return (0);
}
