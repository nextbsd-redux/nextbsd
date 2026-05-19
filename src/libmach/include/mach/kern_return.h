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

#endif /* !_MACH_KERN_RETURN_H_ */
