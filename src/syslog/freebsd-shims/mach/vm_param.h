/* mach/vm_param.h — FreeBSD shim. Apple's VM page-size constants. */
#ifndef _FREEBSD_SHIM_MACH_VM_PARAM_H_
#define _FREEBSD_SHIM_MACH_VM_PARAM_H_

#include <sys/param.h>
#include <unistd.h>

/* On Apple PAGE_SIZE is a compile-time constant (4096 or 16384).
 * On FreeBSD PAGE_SIZE is in <sys/param.h>; provide the Apple
 * spellings as aliases. */
#ifndef vm_page_size
extern long vm_page_size;
#endif

#endif
