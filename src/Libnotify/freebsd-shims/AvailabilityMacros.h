/*
 * AvailabilityMacros.h — FreeBSD shim for Apple's <AvailabilityMacros.h>.
 *
 * The older-style Apple availability header (distinct from the newer
 * <Availability.h>). It defines MAC_OS_X_VERSION_* version constants
 * plus a large family of AVAILABLE_MAC_OS_X_VERSION_*_AND_LATER /
 * DEPRECATED_IN_MAC_OS_X_VERSION_* attribute macros that gate symbols
 * by deployment target.
 *
 * launchd-842's bootstrap.h tags a couple of legacy bootstrap calls
 * with AVAILABLE_..._BUT_DEPRECATED_... attributes. On FreeBSD there
 * is no platform-version gating — every symbol is unconditionally
 * available — so all the attribute macros expand to nothing. The
 * MAC_OS_X_VERSION_* numeric constants are kept (Apple-real values)
 * in case any #if arithmetic references them.
 */
#ifndef _AVAILABILITYMACROS_H_SHIM_
#define _AVAILABILITYMACROS_H_SHIM_

/* Numeric version constants — Apple-real values. */
#define MAC_OS_X_VERSION_10_0	1000
#define MAC_OS_X_VERSION_10_1	1010
#define MAC_OS_X_VERSION_10_2	1020
#define MAC_OS_X_VERSION_10_3	1030
#define MAC_OS_X_VERSION_10_4	1040
#define MAC_OS_X_VERSION_10_5	1050
#define MAC_OS_X_VERSION_10_6	1060
#define MAC_OS_X_VERSION_10_7	1070
#define MAC_OS_X_VERSION_10_8	1080
#define MAC_OS_X_VERSION_10_9	1090
#define MAC_OS_X_VERSION_10_10	101000

/*
 * Attribute macros — all expand to nothing on FreeBSD. The full Apple
 * header has hundreds of AVAILABLE_* / DEPRECATED_* permutations; we
 * provide the ones launchd-842 actually tags symbols with, plus the
 * common base set. Any not-yet-listed permutation an Apple-source
 * file references just needs adding here as a `#define NAME` line.
 */
#define AVAILABLE_MAC_OS_X_VERSION_10_0_AND_LATER
#define AVAILABLE_MAC_OS_X_VERSION_10_1_AND_LATER
#define AVAILABLE_MAC_OS_X_VERSION_10_2_AND_LATER
#define AVAILABLE_MAC_OS_X_VERSION_10_3_AND_LATER
#define AVAILABLE_MAC_OS_X_VERSION_10_4_AND_LATER
#define AVAILABLE_MAC_OS_X_VERSION_10_5_AND_LATER
#define AVAILABLE_MAC_OS_X_VERSION_10_6_AND_LATER
#define AVAILABLE_MAC_OS_X_VERSION_10_7_AND_LATER
#define AVAILABLE_MAC_OS_X_VERSION_10_8_AND_LATER

#define AVAILABLE_MAC_OS_X_VERSION_10_0_AND_LATER_BUT_DEPRECATED_IN_MAC_OS_X_VERSION_10_5
#define AVAILABLE_MAC_OS_X_VERSION_10_0_AND_LATER_BUT_DEPRECATED_IN_MAC_OS_X_VERSION_10_6
#define AVAILABLE_MAC_OS_X_VERSION_10_0_AND_LATER_BUT_DEPRECATED_IN_MAC_OS_X_VERSION_10_7
#define AVAILABLE_MAC_OS_X_VERSION_10_0_AND_LATER_BUT_DEPRECATED_IN_MAC_OS_X_VERSION_10_8

#define DEPRECATED_IN_MAC_OS_X_VERSION_10_5_AND_LATER
#define DEPRECATED_IN_MAC_OS_X_VERSION_10_6_AND_LATER
#define DEPRECATED_IN_MAC_OS_X_VERSION_10_7_AND_LATER
#define DEPRECATED_IN_MAC_OS_X_VERSION_10_8_AND_LATER
#define DEPRECATED_IN_MAC_OS_X_VERSION_10_9_AND_LATER

#define DEPRECATED_ATTRIBUTE
#define UNAVAILABLE_ATTRIBUTE
#define WEAK_IMPORT_ATTRIBUTE

#endif /* !_AVAILABILITYMACROS_H_SHIM_ */
