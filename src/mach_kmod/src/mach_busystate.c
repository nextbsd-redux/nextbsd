/*
 * mach_busystate.c — bus-quiescence busy counter + mach_wait_quiet
 * syscall, layered on the NEXTBSD kernel's balanced
 * device_match_start / device_match_end eventhandler pair.
 *
 * The patched kernel (nextbsd-kernel patches/series:
 * 0002-newbus-device-match-quiesce-hook.patch) brackets every
 * device_probe_and_attach() with EVENTHANDLER_DIRECT_INVOKE of
 * device_match_start on entry and device_match_end on exit. Because the
 * bracketing is single-entry/single-exit it is perfectly paired across
 * every return path and nests under recursion, so a simple ++/--
 * in-flight counter (bus_busy) returns to 0 exactly when the device
 * tree quiesces.
 *
 * We expose that state two ways:
 *
 *   mach.bus.busy        (RD) — current in-flight probe->attach count
 *   mach.bus.quiesce_gen (RD) — bumped once on each 1->0 transition
 *
 * and a mach_wait_quiet syscall (Apple IOKit IOKitWaitQuiet /
 * IOServiceWaitQuiet shape) that blocks until bus_busy == 0.
 *
 * Out-of-tree-only: not part of the ravynOS source tree. Models on the
 * fork eventhandler in src/kern/task.c (registration shape) and on
 * mach_stats.c (the _mach sysctl root).
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/eventhandler.h>
#include <sys/bus.h>
#include <sys/limits.h>		/* INT_MAX */

#include <machine/atomic.h>

#include <sys/sysproto.h>		/* canonical PADL_/PADR_ macros — */
					/* MUST come before _mach_sysproto */
#include <sys/mach/_mach_sysproto.h>	/* mach_wait_quiet_args */
#include <sys/mach/kern_return.h>	/* KERN_SUCCESS */

/*
 * In-flight probe->attach counter. Incremented on device_match_start,
 * decremented on device_match_end; the 1->0 transition bumps
 * bus_quiesce_gen and wakes any thread sleeping in mach_wait_quiet.
 */
static int bus_busy;
static int		bus_quiesce_gen;

static eventhandler_tag	mach_busy_start_tag;
static eventhandler_tag	mach_busy_end_tag;

/*
 * eventhandler callbacks. Signature matches the kernel's
 * device_match_start_fn / device_match_end_fn typedefs
 * (void (*)(void *, device_t)); the device_t is unused — we only
 * track the in-flight depth.
 */
static void
mach_bus_match_start(void *arg __unused, device_t dev __unused)
{
	atomic_add_int(&bus_busy, 1);
}

static void
mach_bus_match_end(void *arg __unused, device_t dev __unused)
{
	if (atomic_fetchadd_int(&bus_busy, -1) == 1) {
		atomic_add_int(&bus_quiesce_gen, 1);
		wakeup(&bus_busy);
	}
}

SYSCTL_DECL(_mach);
static SYSCTL_NODE(_mach, OID_AUTO, bus, CTLFLAG_RD, 0,
    "Bus-quiescence state (device_match_start/end busy counter)");

SYSCTL_INT(_mach_bus, OID_AUTO, busy, CTLFLAG_RD, &bus_busy, 0,
    "Current count of in-flight device_probe_and_attach() calls "
    "(0 == device tree quiescent)");

SYSCTL_INT(_mach_bus, OID_AUTO, quiesce_gen, CTLFLAG_RD,
    &bus_quiesce_gen, 0,
    "Generation counter bumped once on each busy 1->0 transition");

/*
 * mach_wait_quiet — block until the device tree quiesces (bus_busy == 0).
 *
 * uap->timeout is a nanosecond budget (Apple's IOKitWaitQuiet passes a
 * mach_timespec_t; libIOKit converts it to ns). 0 means "no backstop —
 * wait indefinitely" (clamped to INT_MAX ticks so a single tsleep can't
 * overflow the tick argument). On timeout we return KERN_SUCCESS with
 * bus_busy possibly still non-zero — same lenient contract Apple's
 * IOKitWaitQuiet has (it returns kIOReturnSuccess on either quiesce or
 * the caller-supplied deadline elapsing).
 */
int
sys_mach_wait_quiet(struct thread *td, struct mach_wait_quiet_args *uap)
{
	int timo;
	int error;

	if (uap->timeout == 0) {
		timo = INT_MAX;		/* no caller backstop */
	} else {
		/* ns -> ticks; round up so a sub-tick budget waits >= 1 tick. */
		uint64_t ns = uap->timeout;
		uint64_t ticks_budget =
		    (ns + (1000000000ULL / hz) - 1) / (1000000000ULL / hz);

		if (ticks_budget == 0)
			ticks_budget = 1;
		if (ticks_budget > INT_MAX)
			ticks_budget = INT_MAX;
		timo = (int)ticks_budget;
	}

	while (atomic_load_int(&bus_busy) != 0) {
		error = tsleep(&bus_busy, PCATCH, "mwquiet", timo);
		if (error == EWOULDBLOCK)
			break;		/* deadline elapsed — lenient success */
		if (error)
			return (error);	/* EINTR / ERESTART — propagate */
	}

	td->td_retval[0] = KERN_SUCCESS;
	return (0);
}

static void
mach_busystate_sysinit(void *arg __unused)
{
	mach_busy_start_tag = EVENTHANDLER_REGISTER(device_match_start,
	    mach_bus_match_start, NULL, EVENTHANDLER_PRI_ANY);
	mach_busy_end_tag = EVENTHANDLER_REGISTER(device_match_end,
	    mach_bus_match_end, NULL, EVENTHANDLER_PRI_ANY);
}

static void
mach_busystate_sysuninit(void *arg __unused)
{
	if (mach_busy_end_tag != NULL)
		EVENTHANDLER_DEREGISTER(device_match_end, mach_busy_end_tag);
	if (mach_busy_start_tag != NULL)
		EVENTHANDLER_DEREGISTER(device_match_start, mach_busy_start_tag);
}

/* before SI_SUB_INTRINSIC and after SI_SUB_EVENTHANDLER (matches task.c) */
SYSINIT(mach_busystate, SI_SUB_KLD, SI_ORDER_ANY,
    mach_busystate_sysinit, NULL);
SYSUNINIT(mach_busystate, SI_SUB_KLD, SI_ORDER_ANY,
    mach_busystate_sysuninit, NULL);
