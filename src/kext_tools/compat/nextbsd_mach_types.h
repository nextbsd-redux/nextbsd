/*
 * nextbsd_mach_types.h — NextBSD compat (#182).
 *
 * Apple's vendored macho_util.c / fat_util.c use Mach's boolean_t + TRUE/FALSE
 * (normally from <mach/boolean.h>), which FreeBSD does not provide. This header
 * is force-included into ONLY those two files (per-file CFLAGS in the libkext
 * Makefile) so it can't collide with the boolean_t OSKext.c already resolves
 * through its own include chain.
 */
#ifndef _NEXTBSD_MACH_TYPES_H
#define _NEXTBSD_MACH_TYPES_H

#include <stdint.h>

#ifndef _MACH_BOOLEAN_H_
#define _MACH_BOOLEAN_H_
typedef int boolean_t;
#endif

/* Mach VM scalar types fat_util.c/macho_util.c use (<mach/vm_types.h>), absent
 * from the FreeBSD sysroot. Identical-typedef redefinition is allowed in C11 if
 * another compat header also provides them. */
#ifndef _NEXTBSD_VM_TYPES
#define _NEXTBSD_VM_TYPES
typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_address_t;
typedef uintptr_t vm_size_t;
#endif

/* <mach-o/reloc.h>'s relocation_info — referenced by macho_util.c's linkedit
 * trim path. Absent on FreeBSD; the Mach-O bitfield layout is ABI-fixed. */
#ifndef _NEXTBSD_RELOCATION_INFO
#define _NEXTBSD_RELOCATION_INFO
struct relocation_info {
    int32_t  r_address;
    uint32_t r_symbolnum:24,
             r_pcrel:1,
             r_length:2,
             r_extern:1,
             r_type:4;
};
#endif

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#endif /* _NEXTBSD_MACH_TYPES_H */
