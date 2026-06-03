/*
 * freebsd-launchd-mach (2026-05-15)
 *
 * Stub IOKit shim for launchctl. Apple's launchctl uses IOKit
 * for two things, both unrelated to launchctl's core function
 * (load/unload/list/start/stop/etc.):
 *
 *   1. IOKitWaitQuiet — waits for kexts to settle before reboot.
 *      We don't have kexts, but the NEXTBSD kernel + mach.ko track
 *      in-flight device probe->attach (device_match_start/end) and
 *      expose a mach_wait_quiet syscall; this shim resolves + calls it
 *      so launchctl reboot waits for real bus quiescence. Returns
 *      kIOReturnSuccess on quiesce / timeout (or if the syscall is
 *      absent — nothing to wait for).
 *
 *   2. IORegistryEntry{FromPath,CreateCFProperty} + IOObjectRelease
 *      — looks up "IODeviceTree:/chosen" to read kBootRootActiveKey
 *      (does the host boot from a network image?). We always boot
 *      from disk. Stub returns IO_OBJECT_NULL so callers fall through.
 *
 * Apple's launchd-842 IOKit usage is restricted to these four APIs.
 * If launchctl ever picks up more IOKit calls, extend this header.
 */

#ifndef _FREEBSD_LAUNCHD_MACH_IOKIT_IOKITLIB_H_
#define _FREEBSD_LAUNCHD_MACH_IOKIT_IOKITLIB_H_

#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_port.h>

#include <sys/syscall.h>	/* NO_SYSCALL */
#ifndef NO_SYSCALL
#define NO_SYSCALL (-1)	/* kernel sentinel; sysctl reports -1 when a trap is unwired */
#endif
#include <sys/sysctl.h>		/* sysctlbyname */
#include <stdint.h>
#include <stdio.h>		/* snprintf */
#include <unistd.h>		/* syscall(2) */

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque IOKit handle types. macOS aliases these to mach_port_t. */
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_registry_entry_t;

/* Sentinels. */
#define IO_OBJECT_NULL          ((io_object_t)0)
#define kIOMasterPortDefault    ((mach_port_t)0)

/* IOReturn-style status. macOS uses int32_t. */
typedef int32_t IOReturn;
#define kIOReturnSuccess        0

/* Wait-time spec used by IOKitWaitQuiet. macOS struct mach_timespec_t.
 * launchctl only ever passes a pointer to a stack value (or NULL).
 * IOKitWaitQuiet below sums tv_sec/tv_nsec into a nanosecond budget for
 * the mach_wait_quiet syscall. Use plain int fields to avoid pulling in
 * <mach/clock_types.h> (clock_res_t lives there). */
typedef struct {
        unsigned int    tv_sec;
        int             tv_nsec;
} freebsd_mach_timespec_t;
#ifndef mach_timespec_t
#define mach_timespec_t freebsd_mach_timespec_t
#endif

/*
 * IOKitWaitQuiet — wait for the host's device tree to quiesce before
 * launchctl reboot proceeds. On FreeBSD there are no kexts, but the
 * NEXTBSD kernel + mach.ko track in-flight device probe->attach via the
 * device_match_start/device_match_end eventhandlers and expose a
 * `mach_wait_quiet` syscall (resolved by number via `sysctl
 * mach.syscall.mach_wait_quiet`). Resolve + call it here, self-contained
 * (no libIOKit / libmach link dependency for launchctl). `wt` is summed
 * to a nanosecond budget (NULL == wait indefinitely). Returns
 * kIOReturnSuccess on quiesce, deadline, or if the syscall is
 * unavailable (mach.ko not loaded — nothing to wait for, as before).
 */
static inline IOReturn
IOKitWaitQuiet(mach_port_t mp __unused, mach_timespec_t *wt)
{
        int num;
        size_t len = sizeof(num);
        uint64_t timeout_ns;

        if (sysctlbyname("mach.syscall.mach_wait_quiet", &num, &len,
            NULL, 0) != 0 || num < 0 || num == NO_SYSCALL)
                return kIOReturnSuccess;	/* syscall absent — nothing to wait for */
        timeout_ns = (wt == NULL) ? 0
            : ((uint64_t)wt->tv_sec * 1000000000ULL + (uint64_t)wt->tv_nsec);
        (void)syscall(num, timeout_ns);
        return kIOReturnSuccess;
}

/* Stub: no IODeviceTree on FreeBSD; return null so callers exit early. */
static inline io_registry_entry_t
IORegistryEntryFromPath(mach_port_t mp __unused, const char *path __unused)
{
        return IO_OBJECT_NULL;
}

/* Stub: never called — we always return IO_OBJECT_NULL above, so the
 * caller's NULL-check fires before this is reached. Defined for the
 * compiler. */
static inline CFTypeRef
IORegistryEntryCreateCFProperty(io_registry_entry_t entry __unused,
    CFStringRef key __unused, CFAllocatorRef allocator __unused,
    uint32_t options __unused)
{
        return NULL;
}

/* Stub: no-op release for the null handle. */
static inline IOReturn
IOObjectRelease(io_object_t obj __unused)
{
        return kIOReturnSuccess;
}

#ifdef __cplusplus
}
#endif

#endif /* _FREEBSD_LAUNCHD_MACH_IOKIT_IOKITLIB_H_ */
