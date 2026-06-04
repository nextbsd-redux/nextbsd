/*
 * kxld.h (NextBSD compat, nextbsd#182)
 * XNU's in-kernel linker (kxld) is NOT used on NextBSD: the kernel's kld
 * linker links .ko files in-kernel. The kxld function BODIES in OSKext.c are
 * removed (#if 0); these minimal type stubs only let the leftover forward
 * declarations / signatures parse.
 */
#ifndef _NEXTBSD_COMPAT_KXLD_H
#define _NEXTBSD_COMPAT_KXLD_H
#include <stdint.h>
typedef uint64_t kxld_addr_t;
typedef uint64_t kxld_size_t;
typedef struct kxld_context     KXLDContext;
typedef struct kxld_dependency  KXLDDependency;
typedef int  KXLDFlags;
typedef int  KXLDAllocateFlags;
typedef int  KXLDLogLevel;
typedef int  KXLDLogSubsystem;
#endif
