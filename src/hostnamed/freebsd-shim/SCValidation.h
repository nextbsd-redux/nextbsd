/*
 * freebsd-launchd-mach hostnamed freebsd-shim.
 *
 * Apple's SCValidation.h ships a handful of type-check helpers
 * (isA_CFString, isA_CFArray, isA_CFDictionary, ...) — basically
 * "non-NULL AND CFGetTypeID matches X". set-hostname.c uses these
 * to validate dictionary values it reads out of SCDS. The shim
 * provides them as static inlines.
 */

#ifndef _FREEBSD_LAUNCHD_MACH_SC_VALIDATION_H_
#define _FREEBSD_LAUNCHD_MACH_SC_VALIDATION_H_

#include <CoreFoundation/CoreFoundation.h>

#define	_FREEBSD_ISA(NAME, TYPEID_FN)					\
	static inline NAME					 	\
	isA_ ## NAME(CFTypeRef cf)					\
	{								\
		return ((cf != NULL && CFGetTypeID(cf) == TYPEID_FN()) ? \
		    (NAME)cf : NULL);					\
	}

_FREEBSD_ISA(CFStringRef, CFStringGetTypeID)
_FREEBSD_ISA(CFArrayRef, CFArrayGetTypeID)
_FREEBSD_ISA(CFDictionaryRef, CFDictionaryGetTypeID)
_FREEBSD_ISA(CFNumberRef, CFNumberGetTypeID)
_FREEBSD_ISA(CFDataRef, CFDataGetTypeID)
_FREEBSD_ISA(CFBooleanRef, CFBooleanGetTypeID)

#undef	_FREEBSD_ISA

#endif	/* _FREEBSD_LAUNCHD_MACH_SC_VALIDATION_H_ */
