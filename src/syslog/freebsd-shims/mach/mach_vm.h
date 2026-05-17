/* mach/mach_vm.h — FreeBSD shim. Apple's wide-VM Mach calls
 * (mach_vm_allocate / mach_vm_deallocate with 64-bit addresses).
 * libsystem_asl uses for large message-buffer allocation. Map to
 * our libmach which already provides mach_vm_* via mach.h. */
#ifndef _FREEBSD_SHIM_MACH_MACH_VM_H_
#define _FREEBSD_SHIM_MACH_MACH_VM_H_

#include <mach/mach.h>

#endif
