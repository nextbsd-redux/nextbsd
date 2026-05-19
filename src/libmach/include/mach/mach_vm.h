/*
 * mach/mach_vm.h — Apple-canonical Mach VM management API umbrella.
 *
 * libdispatch's internal.h includes this header on the HAVE_MACH path.
 * Two libdispatch sites use it concretely:
 *   - src/data.c: mach_vm_allocate / mach_vm_deallocate for the
 *     dispatch_data large-buffer path
 *   - src/allocator.c: mach_vm_size_t / mach_vm_offset_t /
 *     mach_vm_address_t for magazine math + (under #ifdef) allocation
 *
 * We expose the 64-bit types so consumers parse, and declare the two
 * functions libdispatch references. Real implementations land in a
 * follow-up; expectation today is that consumers either don't reach
 * the calls or get KERN_FAILURE back and fall through.
 */
#ifndef _MACH_MACH_VM_H_
#define _MACH_MACH_VM_H_

#include <mach/mach_types.h>	/* vm_map_t */
#include <mach/kern_return.h>	/* kern_return_t */
#include <stdint.h>

typedef uint64_t mach_vm_address_t;
typedef uint64_t mach_vm_offset_t;
typedef uint64_t mach_vm_size_t;

extern kern_return_t mach_vm_allocate(vm_map_t target,
    mach_vm_address_t *address, mach_vm_size_t size, int flags);
extern kern_return_t mach_vm_deallocate(vm_map_t target,
    mach_vm_address_t address, mach_vm_size_t size);

#endif /* !_MACH_MACH_VM_H_ */
