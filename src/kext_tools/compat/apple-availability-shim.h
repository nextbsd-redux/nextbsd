/* apple-availability-shim.h — NextBSD: neutralize Apple <Availability.h>
 * decorations (#182). FreeBSD has no <Availability.h>, so trailing
 * __OSX_AVAILABLE_*/API_* macros on Apple decls are undefined and clang
 * mis-parses every declarator. Define them as no-op variadics; the
 * __MAC_*/__IPHONE_* version args are swallowed. Force-included via compile-check. */
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
