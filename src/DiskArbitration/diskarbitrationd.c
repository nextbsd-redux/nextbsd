/*
 * diskarbitrationd — freebsd-launchd-mach DiskArbitration daemon.
 *
 * iter 1: daemon skeleton. main + signal handling +
 * bootstrap_check_in for the com.apple.DiskArbitration Mach service
 * + a sleep loop that holds the service port until SIGTERM. No
 * storage subscription yet, no libgeom enrichment, no DA framework
 * surface — iter 2+ wires those.
 *
 * Same shape as ipconfigd / configd / mDNSResponder iter 1:
 * stand the daemon up first, prove the Mach plumbing + launchd
 * MachServices broker + plist + build infra all work, THEN bring in
 * real subsystem code. iter-1 marker DA-BOOT-OK is emitted by the
 * separate `datest` client (bootstrap_look_up against the service).
 *
 * Architecture (per the refactored plan,
 * pkgdemon.github.io/freebsd-disk-arbitration-plan.html):
 *   iter 2  — subscribe to storage device arrival/removal over the
 *             kernel notify channel (ada0/da0/nvd0/cd0 ATTACH /
 *             DETACH), log them.
 *   iter 3  — libgeom enrichment: partition table, UUID, label,
 *             FS-type detection.
 *   iter 4  — DiskArbitration.framework + da.defs MIG IDL serving
 *             DARegisterDisk*Callback / DADiskClaim / DADiskMount /
 *             DADiskUnmount / DADiskEject.
 *   iter 5+ — mount-policy arbitration, per-user agent.
 */
#include "da_iokit_subscribe.h"

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
	(void)fprintf(stderr, "diskarbitrationd %s ", tbuf);

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

	xlog("starting (iter 1 — daemon skeleton, no libgeom enrichment yet)");

	(void)memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	(void)sigaction(SIGTERM, &sa, NULL);
	(void)sigaction(SIGINT, &sa, NULL);
	(void)sigaction(SIGHUP, &sa, NULL);

	/*
	 * Claim the Mach service. Same pattern configd /
	 * ipconfigd / mDNSResponder use: bootstrap_check_in returns the
	 * receive right for the service port that launchd's plist-
	 * declared MachServices broker has set up. Once we hold the
	 * right, peer bootstrap_look_up calls route through launchd to
	 * our receive port — even before we serve real RPC, the lookup
	 * succeeds, which is what the iter-1 DA-BOOT marker proves.
	 */
	kr = bootstrap_check_in(bootstrap_port, "com.apple.DiskArbitration",
	    &svc);
	if (kr != KERN_SUCCESS || svc == MACH_PORT_NULL) {
		xlog("DA-BOOT-FAIL: bootstrap_check_in: 0x%x", (unsigned)kr);
		return (1);
	}
	xlog("DA-BOOT-OK: com.apple.DiskArbitration registered "
	    "(receive right=0x%x)", (unsigned)svc);

	/*
	 * Subscribe to storage device arrival/removal over the kernel notify
	 * channel (C1.3, #218): libIOKit's IOServiceAddMatchingNotification
	 * registers DA's recv port with the in-kernel registry via IOREGIOCWATCH
	 * on /dev/ioregistry (#225), and the kernel pushes a binary event per
	 * matching device. DA registers a match-all arrival + departure watch and
	 * filters storage names (ada*, da*, nvd*, nda*, cd*, mmcsd*) client-side.
	 *
	 * Non-fatal: if /dev/ioregistry is absent (a kernel image predating K1) or
	 * registration fails, da_iokit_subscribe_start returns -1 and the daemon
	 * stays alive without storage events — KeepAlive respawns / relookups
	 * happen as services appear. (hwregd, the former fallback, was retired in
	 * #218; the kernel is now the registry.)
	 */
	(void)da_iokit_subscribe_start();

	/*
	 * iter-1 hold-alive loop. Spin on sleep(60) — SIGTERM/SIGHUP
	 * short-circuit. iter 3+ replaces this with a Mach receive loop
	 * demuxing the da.defs MIG IDL (DARegisterDisk*Callback etc.).
	 */
	while (!got_term)
		(void)sleep(60);

	xlog("exiting on signal %d", (int)got_term);
	return (0);
}
