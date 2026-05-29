/*
 * Copyright (c) 2000-2023 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

/*
 * SCPName.c — SCPreferences-side name reader.
 *
 * _SCPreferencesCopyComputerName: opens the prefs path /System/System
 * (the dict that carries the user-set ComputerName), returns the
 * ComputerName string. If `nameEncoding` is non-NULL, writes the
 * stored ComputerNameEncoding (or kCFStringEncodingUTF8 when absent).
 *
 * Mirrors Apple's SystemConfiguration.fproj/SCDHostName.c contract.
 * The "_SC" prefix marks this as a private SPI: Apple's set-hostname.c
 * calls it directly; downstream consumers should prefer the public
 * SCDynamicStoreCopyComputerName variant in SCDName.c.
 */

#include <SystemConfiguration/SCPreferences.h>
#include <SystemConfiguration/SCSchemaDefinitions.h>

#include <CoreFoundation/CoreFoundation.h>

CFStringRef
_SCPreferencesCopyComputerName(SCPreferencesRef prefs,
    CFStringEncoding *nameEncoding)
{
	CFStringRef name = NULL;
	CFStringRef path;
	CFDictionaryRef sysdict;

	if (nameEncoding != NULL)
		*nameEncoding = kCFStringEncodingUTF8;
	if (prefs == NULL)
		return (NULL);

	path = CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@/%@"),
	    kSCCompSystem, kSCCompSystem);
	if (path == NULL)
		return (NULL);
	sysdict = SCPreferencesPathGetValue(prefs, path);
	CFRelease(path);
	if (sysdict == NULL ||
	    CFGetTypeID(sysdict) != CFDictionaryGetTypeID())
		return (NULL);

	{
		CFStringRef v = CFDictionaryGetValue(sysdict,
		    kSCPropSystemComputerName);
		if (v != NULL && CFGetTypeID(v) == CFStringGetTypeID())
			name = CFStringCreateCopy(NULL, v);
	}
	if (nameEncoding != NULL) {
		CFNumberRef enc = CFDictionaryGetValue(sysdict,
		    kSCPropSystemComputerNameEncoding);
		if (enc != NULL && CFGetTypeID(enc) == CFNumberGetTypeID()) {
			SInt32 v32;
			if (CFNumberGetValue(enc, kCFNumberSInt32Type, &v32))
				*nameEncoding = (CFStringEncoding)v32;
		}
	}
	return (name);
}
