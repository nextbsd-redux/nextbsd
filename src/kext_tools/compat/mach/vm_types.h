/* mach/vm_types.h — NextBSD compat: Mach VM scalar types (#182). */
#ifndef _NEXTBSD_COMPAT_MACH_VM_TYPES_H
#define _NEXTBSD_COMPAT_MACH_VM_TYPES_H
#include <sys/types.h>
#include <stdint.h>
#ifndef _VM_OFFSET_T_DEFINED
#define _VM_OFFSET_T_DEFINED
typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
typedef uint64_t  mach_vm_address_t;
typedef uint64_t  mach_vm_size_t;
typedef uint64_t  mach_vm_offset_t;
typedef void     *vm_map_t;
#endif
#endif
