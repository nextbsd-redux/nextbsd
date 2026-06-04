/*
 * IOKit/IOKitServer.h — NextBSD compat stub for the OSKext port (#182, Phase 0).
 * XNU kext_request server iface — re-backed to kld.
 * Minimal placeholder so the #include resolves; populated per the
 * compile-check error surface, or the consuming code path is #ifdef-ed
 * out when it is XNU-only (kxld / mkext / prelink / kext_request).
 */
#ifndef _NEXTBSD_COMPAT_IOKITSERVER_H
#define _NEXTBSD_COMPAT_IOKITSERVER_H

/* Standard IOKit personality / matching dictionary keys (IOKitKeys.h on XNU).
 * OSKext references these when building/serializing driver personalities; the
 * values are the real IOKit property names the matching machinery uses. */
#define kIOKitDebugKey       "IOKitDebug"
#define kIOProbeScoreKey     "IOProbeScore"
#define kIOMatchCategoryKey  "IOMatchCategory"
#define kIOResourcesClass    "IOResources"

#endif
