/*
 * apple-availability-shim.h (NextBSD, nextbsd#182)
 *
 * FreeBSD has no Availability.h, so the trailing OSX-availability and API
 * availability macros on Apple declarations are undefined and clang
 * mis-parses every declarator. Define them as no-op variadic macros; the
 * version-constant arguments are swallowed, so they need no values.
 * Force-included by the kext_tools build (via the Makefiles).
 */
#ifndef _NEXTBSD_APPLE_AVAILABILITY_SHIM_H
#define _NEXTBSD_APPLE_AVAILABILITY_SHIM_H
#define __OSX_AVAILABLE_STARTING(...)
#define __OSX_AVAILABLE_BUT_DEPRECATED(...)
#define __OSX_AVAILABLE_BUT_DEPRECATED_MSG(...)
#define __OSX_AVAILABLE(...)
#define __OSX_DEPRECATED(...)
#define __API_AVAILABLE(...)
#define __API_DEPRECATED(...)
#define __API_UNAVAILABLE(...)
#define API_AVAILABLE(...)
#define API_DEPRECATED(...)
#define API_DEPRECATED_WITH_REPLACEMENT(...)
#define API_UNAVAILABLE(...)
#endif
