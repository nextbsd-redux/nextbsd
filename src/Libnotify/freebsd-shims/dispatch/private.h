/* dispatch/private.h — FreeBSD shim.
 *
 * Chain to the real Apple-private dispatch headers installed by our
 * libdispatch port (at ${SYSROOT}/usr/include/dispatch/), so notifyd
 * and friends pick up the actual function declarations and types
 * (dispatch_workloop_t, dispatch_mach_t, dispatch_mach_create_f,
 * dispatch_workloop_create_inactive, ...).
 *
 * History: earlier J2 iterations stubbed these as #defines returning
 * NULL because notifyd didn't yet run far enough to exercise them.
 * With the Path A bootstrap chain landed (task #39), notifyd now
 * reaches dispatch_workloop_create_inactive at startup; the NULL
 * stub crashed the daemon when it dereferenced the returned NULL.
 * The real implementations exist in libdispatch.so.
 */
#ifndef _FREEBSD_SHIM_DISPATCH_PRIVATE_H_
#define _FREEBSD_SHIM_DISPATCH_PRIVATE_H_

/* libdispatch's private.h gates dispatch/mach_private.h on this. */
#ifndef DISPATCH_MACH_SPI
#define DISPATCH_MACH_SPI 1
#endif

#include_next <dispatch/private.h>

/* QoS class macros normally provided by <pthread/qos.h> on macOS.
 * FreeBSD has no equivalent; libdispatch treats these as opaque
 * integer values passed back into the kernel-side workloop config. */
#ifndef QOS_CLASS_USER_INTERACTIVE
#define QOS_CLASS_USER_INTERACTIVE	0x21
#define QOS_CLASS_USER_INITIATED	0x19
#define QOS_CLASS_DEFAULT		0x15
#define QOS_CLASS_UTILITY		0x11
#define QOS_CLASS_BACKGROUND		0x09
#define QOS_CLASS_UNSPECIFIED		0x00
#endif

#endif
