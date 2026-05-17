/* mach/vm_statistics.h — FreeBSD shim. Apple's Mach VM statistics
 * structs (struct vm_statistics64, host_statistics64()). libsystem_asl
 * pulls this in transitively but doesn't actually use the stats —
 * just needs the include to resolve. Provide a minimal opaque type. */
#ifndef _FREEBSD_SHIM_MACH_VM_STATISTICS_H_
#define _FREEBSD_SHIM_MACH_VM_STATISTICS_H_

#include <stdint.h>

/* Minimal placeholder for vm_statistics64_data_t — defined only
 * to satisfy struct-pointer references. Real Apple version has
 * dozens of page-count fields. */
struct vm_statistics64 {
	uint64_t	free_count;
	uint64_t	active_count;
	uint64_t	inactive_count;
	uint64_t	wire_count;
};
typedef struct vm_statistics64	vm_statistics64_data_t;
typedef struct vm_statistics64 *vm_statistics64_t;

/* Apple's VM_FLAGS_* options for vm_allocate. Unused by libsystem_asl;
 * declared so transitive includes compile. */
#define VM_FLAGS_ANYWHERE	0x0001
#define VM_FLAGS_FIXED		0x0000
#define VM_FLAGS_PURGABLE	0x0002

#endif
