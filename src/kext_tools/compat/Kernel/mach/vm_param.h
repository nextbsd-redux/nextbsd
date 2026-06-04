/*
 * Kernel/mach/vm_param.h — NextBSD compat stub for the OSKext port (#182, Phase 0).
 * page-size constants.
 * Minimal placeholder so the #include resolves; populated per the
 * compile-check error surface, or the consuming code path is #ifdef-ed
 * out when it is XNU-only (kxld / mkext / prelink / kext_request).
 */
#ifndef _NEXTBSD_COMPAT_VM_PARAM_H
#define _NEXTBSD_COMPAT_VM_PARAM_H
#include <sys/param.h>
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#ifndef PAGE_MASK
#define PAGE_MASK (PAGE_SIZE - 1)
#endif
#ifndef round_page
#define round_page(x) (((x) + PAGE_MASK) & ~PAGE_MASK)
#endif
#endif
