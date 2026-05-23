/*
 * lease_loop.c — post-BOUND lease management. RFC 2131 §4.4.5
 * BOUND/RENEWING/REBINDING state-machine on the daemon's main
 * thread.
 *
 * Sleep is 1s-tick polling on `got_term` so SIGTERM shortens any
 * wait; the longest contiguous block call is sleep(1).
 */
#include "lease_loop.h"
#include "dhcp_discover.h"
#include "sc_publish.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
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

int
lease_loop_run(const char *ifname, struct dhcp_lease *lease,
    struct sc_publish *pub)
{
	uint32_t lease_secs = lease->lease_time;

	if (lease_secs == 0) {
		xlog("zero lease time — treating as infinite (no renewal)");
		while (!got_term)
			(void)sleep(60);
		return (0);
	}

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
			lease_secs = lease->lease_time;
			if (pub != NULL &&
			    sc_publish_ipv4(pub, ifname, lease) != 0)
				xlog("RENEWING: re-publish failed (continuing)");
			xlog("RENEWING -> BOUND (new lease=%us)", lease_secs);
			continue;
		}
		xlog("RENEWING: no ACK; waiting until T2");

		if (sleep_interruptible(t2_after_t1) < 0)
			return (0);

		/* REBINDING. */
		xlog("REBINDING: T2 fired, sending DHCPREQUEST");
		if (dhcp_renew(ifname, lease, &renewed) == 0) {
			*lease = renewed;
			lease_secs = lease->lease_time;
			if (pub != NULL &&
			    sc_publish_ipv4(pub, ifname, lease) != 0)
				xlog("REBINDING: re-publish failed (continuing)");
			xlog("REBINDING -> BOUND (new lease=%us)", lease_secs);
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
