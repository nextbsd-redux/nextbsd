/*
 * mach/vm_map.h — minimal userland VM-map surface.
 *
 * Apple's <mach/vm_map.h> is the full VM RPC interface (vm_allocate,
 * vm_deallocate, vm_read, vm_map, ...). launchd-842's liblaunch only
 * reaches for vm_deallocate (freeing MIG out-of-line buffers) and the
 * mach_vm_address_t type. We provide just that surface; the rest of
 * Apple's VM RPC is not wired into mach.ko and would need real kernel
 * work to support.
 */
#ifndef _MACH_VM_MAP_H_
#define _MACH_VM_MAP_H_

#include <stdint.h>
#include <sys/types.h>		/* FreeBSD's vm_offset_t / vm_size_t */
#include <mach/mach_traps.h>	/* mach_port_name_t */
#include <mach/kern_return.h>	/* kern_return_t */
#include <mach/std_types.h>	/* natural_t */

#ifdef __cplusplus
extern "C" {
#endif

/* FreeBSD's <sys/types.h> already provides vm_offset_t and vm_size_t
 * (guarded by _VM_OFFSET_T_DECLARED / _VM_SIZE_T_DECLARED). We only
 * add the Apple-specific names FreeBSD lacks: vm_address_t and the
 * mach_vm_* 64-bit-wide variants. Each is individually guarded so a
 * later real Mach VM header can take over without a clash. */
#ifndef _VM_ADDRESS_T_DECLARED
#define _VM_ADDRESS_T_DECLARED
typedef unsigned long	vm_address_t;
#endif
#ifndef _MACH_VM_TYPES_DECLARED
#define _MACH_VM_TYPES_DECLARED
typedef uint64_t	mach_vm_address_t;
typedef uint64_t	mach_vm_size_t;
typedef uint64_t	mach_vm_offset_t;
#endif

/*
 * vm_map_t REMOVED. FreeBSD's <vm/vm.h> (line 107) unconditionally
 * declares `typedef struct vm_map *vm_map_t;` and any libdispatch TU
 * that includes <sys/user.h> (workqueue.c does, for kinfo_proc) pulls
 * it in, conflicting with our `typedef mach_port_name_t vm_map_t`.
 * No userland consumer in this tree uses the Apple name outside the
 * libmach stubs themselves (grep src/libdispatch src/libxpc src/launchd
 * src/syslog — empty). Use mach_port_name_t directly in the API.
 *
 * Future callers that need Apple's vm_map_t source-shape can locally
 * `typedef mach_port_name_t vm_map_t;` in a TU that doesn't pull in
 * <vm/vm.h>.
 */

/*
 * vm_deallocate — release a region of a task's VM. liblaunch calls
 * this to free the out-of-line memory MIG hands back from a reply.
 * mach.ko does not implement the VM RPC subsystem yet; the libmach
 * userland stub returns KERN_SUCCESS (no-op) so callers fail-soft
 * rather than link-fail. Promote to a real trap when OOL message
 * memory actually needs reclaiming.
 */
kern_return_t vm_deallocate(mach_port_name_t target, vm_address_t address,
    vm_size_t size);

kern_return_t vm_allocate(mach_port_name_t target, vm_address_t *address,
    vm_size_t size, int flags);

#ifdef __cplusplus
}
#endif

#endif /* !_MACH_VM_MAP_H_ */
