/*
 * mach/kern_return.h — Apple-canonical kern_return_t header.
 *
 * Apple-source code does `#include <mach/kern_return.h>` for the
 * kern_return_t type. We already define kern_return_t in
 * <mach/mach_port.h> and <mach/message.h> under the
 * _KERN_RETURN_T_DEFINED guard; this header is the canonical
 * standalone home. The full KERN_* code set lives kernel-side in
 * <sys/mach/kern_return.h>; the handful of codes our userland needs
 * are in <mach/mach_port.h>. migcom only uses the type, not the
 * codes, so the typedef alone is enough here.
 */
#ifndef _MACH_KERN_RETURN_H_
#define _MACH_KERN_RETURN_H_

#ifndef _KERN_RETURN_T_DEFINED
#define _KERN_RETURN_T_DEFINED
typedef int kern_return_t;
#endif

/*
 * KERN_OPERATION_TIMED_OUT — libdispatch's shims/lock.c (line 182,
 * line 238 _DSEMA4_TIMEOUT) and HAVE_MACH semaphore.c paths compare
 * kr against this constant. Apple defines it as 49 in
 * <mach/kern_return.h>. We carry the same value.
 */
#ifndef KERN_OPERATION_TIMED_OUT
#define KERN_OPERATION_TIMED_OUT 49
#endif

/*
 * KERN_NOT_SUPPORTED — libdispatch's voucher.c (1888, 1898) returns
 * this from the voucher activity_trace / personas APIs we don't
 * implement. Apple's canonical value is 46.
 */
#ifndef KERN_NOT_SUPPORTED
#define KERN_NOT_SUPPORTED 46
#endif

/*
 * KERN_FAILURE / KERN_SUCCESS — the two most-used codes. We already
 * carry KERN_SUCCESS via the Apple-canonical 0 fallback elsewhere
 * (mach_port.h, message.h), but libdispatch's voucher.c and other
 * paths reference them explicitly with this header alone in scope.
 */
#ifndef KERN_SUCCESS
#define KERN_SUCCESS 0
#endif
#ifndef KERN_FAILURE
#define KERN_FAILURE 5
#endif

/*
 * KERN_ABORTED — Mach semaphore wait returns this when interrupted
 * before the timeout. libdispatch's shims/lock.c (165, 180) treats it
 * as a retry cue. Apple's value is 14.
 */
#ifndef KERN_ABORTED
#define KERN_ABORTED 14
#endif

#endif /* !_MACH_KERN_RETURN_H_ */
