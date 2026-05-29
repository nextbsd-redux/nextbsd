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

/* Apple's convention: isA_CFString / isA_CFArray / ... — NO "Ref"
 * suffix on the function name, even though the return type IS the
 * Ref. The two macro arguments split the name (no-Ref) from the
 * type (with-Ref). */
#define	_FREEBSD_ISA(BASE, TYPE, TYPEID_FN)				\
	static inline TYPE					 	\
	isA_ ## BASE(CFTypeRef cf)					\
	{								\
		return ((cf != NULL && CFGetTypeID(cf) == TYPEID_FN()) ? \
		    (TYPE)cf : NULL);					\
	}

_FREEBSD_ISA(CFString, CFStringRef, CFStringGetTypeID)
_FREEBSD_ISA(CFArray, CFArrayRef, CFArrayGetTypeID)
_FREEBSD_ISA(CFDictionary, CFDictionaryRef, CFDictionaryGetTypeID)
_FREEBSD_ISA(CFNumber, CFNumberRef, CFNumberGetTypeID)
_FREEBSD_ISA(CFData, CFDataRef, CFDataGetTypeID)
_FREEBSD_ISA(CFBoolean, CFBooleanRef, CFBooleanGetTypeID)

#undef	_FREEBSD_ISA

#endif	/* _FREEBSD_LAUNCHD_MACH_SC_VALIDATION_H_ */
