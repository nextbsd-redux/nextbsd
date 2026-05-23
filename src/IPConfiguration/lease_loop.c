/*
 * lease_loop.c — post-BOUND lease management. RFC 2131 §4.4.5
 * BOUND/RENEWING/REBINDING state-machine on the daemon's main
 * thread.
 *
 * Sleep is 1s-tick polling on `got_term` so SIGTERM shortens any
 * wait; the longest contiguous block call is sleep(1).
 *
 * iter 5b: lease_cap_secs (from IPCONFIGD_FAST_LEASE in env) caps
 * the effective lease time used to derive T1 / T2. Useful in CI
 * where SLIRP hands out 86400s leases that would otherwise mean
 * T1 = 12 hours; capping to e.g. 10s makes the renewal code run
 * within the boot-test budget. The cap is NOT applied to
 * lease->lease_time (the value sc_publish.c forwards to configd) —
 * downstream consumers see the server's authoritative value. iter
 * 5b also emits IPCFG-RENEW-OK once on the first successful
 * RENEWING/REBINDING ACK so the gate fires deterministically.
 */
#include "lease_loop.h"
#include "dhcp_discover.h"
#include "sc_publish.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "ipconfigd[lease] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/*
 * Sleep for `seconds` in 1-second ticks, returning early if
 * `got_term` is asserted. Returns 0 on full wait, -1 if interrupted.
 */
static int
sleep_interruptible(uint32_t seconds)
{
	uint32_t i;

	for (i = 0; i < seconds; i++) {
		if (got_term)
			return (-1);
		(void)sleep(1);
	}
	return (got_term ? -1 : 0);
}

/*
 * Apply the iter-5b lease cap. Returns min(in, cap) when cap > 0;
 * `in` unchanged otherwise. The cap only shortens timer math —
 * lease->lease_time (the value published to configd) is untouched.
 */
static uint32_t
cap_lease(uint32_t in, uint32_t cap)
{
	if (cap > 0 && in > cap)
		return (cap);
	return (in);
}

int
lease_loop_run(const char *ifname, struct dhcp_lease *lease,
    struct sc_publish *pub, uint32_t lease_cap_secs)
{
	uint32_t lease_secs = cap_lease(lease->lease_time, lease_cap_secs);
	bool renew_marker_fired = false;

	if (lease_secs == 0) {
		xlog("zero lease time — treating as infinite (no renewal)");
		while (!got_term)
			(void)sleep(60);
		return (0);
	}
	if (lease_cap_secs > 0)
		xlog("lease cap active: server lease=%us → effective=%us",
		    (unsigned)lease->lease_time, lease_secs);

	for (;;) {
		uint32_t t1, t2_after_t1, expiry_after_t2;
		struct dhcp_lease renewed;

		/*
		 * RFC 2131 §4.4.5:
		 *   T1 = lease/2
		 *   T2 = 0.875 * lease
		 * Use integer math: T2 - T1 ≈ lease*3/8. Expiry - T2 ≈ lease/8.
		 */
		t1 = lease_secs / 2;
		t2_after_t1 = (lease_secs * 3) / 8;
		expiry_after_t2 = lease_secs - (t1 + t2_after_t1);

		xlog("BOUND: lease=%us T1=+%us T2=+%us expiry=+%us",
		    lease_secs, t1, t1 + t2_after_t1, lease_secs);

		if (sleep_interruptible(t1) < 0)
			return (0);

		/* RENEWING. */
		xlog("RENEWING: T1 fired, sending DHCPREQUEST");
		if (dhcp_renew(ifname, lease, &renewed) == 0) {
			*lease = renewed;
			lease_secs = cap_lease(lease->lease_time,
			    lease_cap_secs);
			if (pub != NULL &&
			    sc_publish_ipv4(pub, ifname, lease) != 0)
				xlog("RENEWING: re-publish failed (continuing)");
			xlog("RENEWING -> BOUND (server lease=%us, "
			    "effective=%us)",
			    (unsigned)lease->lease_time, lease_secs);
			if (!renew_marker_fired) {
				xlog("IPCFG-RENEW-OK");
				renew_marker_fired = true;
			}
			continue;
		}
		xlog("RENEWING: no ACK; waiting until T2");

		if (sleep_interruptible(t2_after_t1) < 0)
			return (0);

		/* REBINDING. */
		xlog("REBINDING: T2 fired, sending DHCPREQUEST");
		if (dhcp_renew(ifname, lease, &renewed) == 0) {
			*lease = renewed;
			lease_secs = cap_lease(lease->lease_time,
			    lease_cap_secs);
			if (pub != NULL &&
			    sc_publish_ipv4(pub, ifname, lease) != 0)
				xlog("REBINDING: re-publish failed (continuing)");
			xlog("REBINDING -> BOUND (server lease=%us, "
			    "effective=%us)",
			    (unsigned)lease->lease_time, lease_secs);
			if (!renew_marker_fired) {
				xlog("IPCFG-RENEW-OK");
				renew_marker_fired = true;
			}
			continue;
		}
		xlog("REBINDING: no ACK; waiting until lease expiry");

		if (sleep_interruptible(expiry_after_t2) < 0)
			return (0);

		xlog("lease expired without renewal — unpublishing");
		if (pub != NULL)
			(void)sc_publish_remove(pub, ifname);
		return (-1);
	}
}
