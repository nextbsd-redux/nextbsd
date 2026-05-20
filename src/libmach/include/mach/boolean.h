/*
 * mach/boolean.h — Apple-canonical boolean type header.
 *
 * Apple-source code (migcom, MIG-generated stubs, launchd-842) does
 * `#include <mach/boolean.h>` for boolean_t. We already define
 * boolean_t in <mach/message.h> under the _BOOLEAN_T_DEFINED guard;
 * this header is the canonical standalone home so the include
 * resolves on its own.
 */
#ifndef _MACH_BOOLEAN_H_
#define _MACH_BOOLEAN_H_

/*
 * HAVE_BOOLEAN tells FreeBSD's <vm/vm.h> (line 121) to skip its own
 * `typedef int boolean_t;`. Otherwise libdispatch's workqueue.c
 * (which includes <sys/user.h> → <vm/vm.h>) ends up with two
 * conflicting boolean_t typedefs — ours (unsigned int) and FreeBSD's
 * (int). Definition order matters: HAVE_BOOLEAN must be set before
 * the system include chain runs. Since libmach's <mach/mach.h>
 * pulls boolean.h first, this is the earliest reliable point.
 */
#ifndef HAVE_BOOLEAN
#define HAVE_BOOLEAN 1
#endif
#ifndef _BOOLEAN_T_DEFINED
#define _BOOLEAN_T_DEFINED
typedef unsigned int boolean_t;
#endif

#ifndef TRUE
#define TRUE	1
#endif
#ifndef FALSE
#define FALSE	0
#endif

#endif /* !_MACH_BOOLEAN_H_ */
