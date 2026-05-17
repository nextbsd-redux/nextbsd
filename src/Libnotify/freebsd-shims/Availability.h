/*
 * Availability.h — local stub for Apple's <Availability.h>.
 *
 * libxpc's public headers (xpc/base.h, xpc/activity.h, xpc/connection.h,
 * ...) include <Availability.h> and use macros like
 * __OSX_AVAILABLE_STARTING(__MAC_10_9, __IPHONE_7_0). On Apple
 * platforms these expand to compiler attributes that gate the symbol
 * by macOS/iOS version. On FreeBSD they have no meaning — every
 * symbol is unconditionally available — so we expand them to nothing.
 *
 * Reachable via the Makefile's -I${.CURDIR}, the same way uuid/uuid.h
 * is shimmed.
 */
#ifndef _LIBXPC_AVAILABILITY_H_SHIM_
#define _LIBXPC_AVAILABILITY_H_SHIM_

/* The __MAC_* / __IPHONE_* version constants must carry their real
 * NUMERIC values, not expand to nothing — Apple-source headers do
 * arithmetic with them inside #if expressions, e.g.
 *   #if __MAC_OS_X_VERSION_MIN_REQUIRED < __MAC_10_9
 * An empty expansion leaves "0 < " and the preprocessor errors with
 * "expected value in expression" (hit in dispatch/queue.h). The
 * values below match Apple's real <AvailabilityVersions.h>. The
 * *_VERSION_MIN_REQUIRED / *_VERSION_MAX_ALLOWED macros are left
 * undefined (so they read as 0 in #if), which makes every
 * "min_required < __MAC_x" test false and every
 * "max_allowed >= __MAC_x" test false — i.e. no platform-version
 * gating fires, the correct behavior for a non-Apple target. */
#define __MAC_10_0	1000
#define __MAC_10_1	1010
#define __MAC_10_2	1020
#define __MAC_10_3	1030
#define __MAC_10_4	1040
#define __MAC_10_5	1050
#define __MAC_10_6	1060
#define __MAC_10_7	1070
#define __MAC_10_8	1080
#define __MAC_10_9	1090
#define __MAC_10_10	101000
#define __MAC_10_11	101100
#define __MAC_10_12	101200
#define __MAC_10_13	101300
#define __MAC_10_14	101400
#define __MAC_10_15	101500
#define __IPHONE_1_0	10000
#define __IPHONE_2_0	20000
#define __IPHONE_3_0	30000
#define __IPHONE_4_0	40000
#define __IPHONE_5_0	50000
#define __IPHONE_6_0	60000
#define __IPHONE_7_0	70000
#define __IPHONE_8_0	80000
#define __IPHONE_NA	99999

#define __OSX_AVAILABLE_STARTING(...)
#define __OSX_AVAILABLE_BUT_DEPRECATED(...)
#define __OSX_AVAILABLE_BUT_DEPRECATED_MSG(...)
#define __OSX_AVAILABLE(...)
#define __OSX_DEPRECATED(...)
#define __IOS_AVAILABLE(...)
#define __IOS_DEPRECATED(...)
#define __WATCHOS_AVAILABLE(...)
#define __WATCHOS_DEPRECATED(...)
#define __TVOS_AVAILABLE(...)
#define __TVOS_DEPRECATED(...)
#define API_AVAILABLE(...)
#define API_DEPRECATED(...)
#define API_DEPRECATED_WITH_REPLACEMENT(...)
#define API_UNAVAILABLE(...)
#define SPI_AVAILABLE(...)

#endif /* !_LIBXPC_AVAILABILITY_H_SHIM_ */
