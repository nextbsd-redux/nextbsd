/*
 * System/libkern/mkext.h — NextBSD compat stub for the OSKext port (#182, Phase 0).
 * mkext archive format — DELETED (kld loads .ko directly).
 * Minimal placeholder so the #include resolves; populated per the
 * compile-check error surface, or the consuming code path is #ifdef-ed
 * out when it is XNU-only (kxld / mkext / prelink / kext_request).
 */
#ifndef _NEXTBSD_COMPAT_MKEXT_H
#define _NEXTBSD_COMPAT_MKEXT_H

/* Info-dictionary keys that XNU sets on a kext extracted from an mkext archive.
 * NextBSD has no mkext (kld loads .ko directly), so these keys are never
 * present at runtime; defined only so the now-inert lookups in OSKext.c
 * (e.g. "did this kext come from an mkext?") parse. */
#define kMKEXTBundlePathKey  "MKEXTBundlePath"
#define kMKEXTExecutableKey  "MKEXTExecutable"

#endif
