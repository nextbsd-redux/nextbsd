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

#ifndef _MACH_BOOLEAN_H_
#define _MACH_BOOLEAN_H_
typedef int boolean_t;
#endif

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#endif /* _NEXTBSD_MACH_TYPES_H */
