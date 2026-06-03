/*
 * test_busystate — validate the bus-quiescence feature mach.ko exposes
 * on top of the NEXTBSD kernel's device_match_start/device_match_end
 * eventhandler pair.
 *
 * Two checks, two markers (printed for the boot-test expect harness):
 *
 *   BUSYSTATE-OK / BUSYSTATE-FAIL
 *     Read `sysctl mach.bus.busy`. By the time run.sh executes (well
 *     after the cold-boot device probe has finished and hwregd has
 *     flipped to live mode) the device tree is quiescent, so the count
 *     must read 0. We poll a few times to absorb any late, transient
 *     probe (e.g. a deferred kldload's attach).
 *
 *   WAITQUIET-OK / WAITQUIET-FAIL
 *     Resolve mach_wait_quiet via `sysctl mach.syscall.mach_wait_quiet`
 *     and call it with a 1s nanosecond budget. Because the bus is
 *     already quiescent it must return promptly (well under the budget)
 *     with rc 0.
 *
 * Exit codes:
 *   0 — both checks passed
 *   1 — mach.bus.busy sysctl unavailable
 *   2 — mach.bus.busy never settled to 0
 *   3 — mach.syscall.mach_wait_quiet unavailable
 *   4 — mach_wait_quiet returned non-zero / didn't return promptly
 */
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

int
main(void)
{
	int busy = -1;
	size_t len = sizeof(busy);
	int i;

	/* 1. mach.bus.busy must exist and settle to 0. */
	for (i = 0; i < 50; i++) {
		len = sizeof(busy);
		if (sysctlbyname("mach.bus.busy", &busy, &len, NULL, 0) != 0) {
			printf("BUSYSTATE-FAIL: sysctl mach.bus.busy "
			    "unavailable (mach.ko not loaded / no "
			    "device_match hook?)\n");
			return (1);
		}
		printf("mach.bus.busy = %d\n", busy);
		if (busy == 0)
			break;
		usleep(100000);	/* 100ms — absorb a late transient probe */
	}
	if (busy != 0) {
		printf("BUSYSTATE-FAIL: mach.bus.busy never settled to 0 "
		    "(last=%d)\n", busy);
		return (2);
	}
	printf("BUSYSTATE-OK: mach.bus.busy == 0 (device tree quiescent)\n");

	/* 2. mach_wait_quiet must resolve and return promptly. */
	int num;
	len = sizeof(num);
	if (sysctlbyname("mach.syscall.mach_wait_quiet", &num, &len,
	    NULL, 0) != 0 || num < 0) {
		printf("WAITQUIET-FAIL: sysctl mach.syscall.mach_wait_quiet "
		    "unavailable\n");
		return (3);
	}
	printf("mach.syscall.mach_wait_quiet = %d\n", num);

	struct timespec t0, t1;
	(void)clock_gettime(CLOCK_MONOTONIC, &t0);
	/* 1s budget in ns — quiescent bus should return well under it. */
	long rc = syscall(num, (uint64_t)1000000000ULL);
	(void)clock_gettime(CLOCK_MONOTONIC, &t1);

	double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
	    (t1.tv_nsec - t0.tv_nsec) / 1000000.0;
	printf("mach_wait_quiet rc=%ld elapsed=%.1fms\n", rc, ms);

	if (rc != 0) {
		printf("WAITQUIET-FAIL: mach_wait_quiet returned %ld\n", rc);
		return (4);
	}
	if (ms > 500.0) {
		printf("WAITQUIET-FAIL: mach_wait_quiet blocked %.1fms on an "
		    "already-quiescent bus\n", ms);
		return (4);
	}
	printf("WAITQUIET-OK: mach_wait_quiet returned promptly (%.1fms)\n", ms);
	return (0);
}
