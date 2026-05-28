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
 * SCDName.c — the SCDynamicStore name-reader convenience helpers.
 *
 * SCDynamicStoreCopyLocalHostName: reads Setup:/Network/HostNames
 * (the dict mDNSResponder owns LocalHostName in) and pulls out the
 * Bonjour name. Returns CFString or NULL.
 *
 * SCDynamicStoreCopyComputerName: reads Setup:/System and pulls out
 * ComputerName + ComputerNameEncoding. Returns CFString and (if the
 * caller asked) the encoding. Both publish surfaces are owned by
 * PreferencesMonitor / hostnamed's prefs_monitor equivalent.
 *
 * freebsd-launchd-mach port: mirrors the contract of Apple's
 * SystemConfiguration.fproj/SCDHostName.c. Trimmed — Apple's file
 * also has Set-side variants (SCPreferencesSetLocalHostName, etc.)
 * that we will add when iter 4 (Bonjour conflict-rename) needs them.
 */

#include <SystemConfiguration/SCDynamicStore.h>
#include <SystemConfiguration/SCSchemaDefinitions.h>

#include <CoreFoundation/CoreFoundation.h>

CFStringRef
SCDynamicStoreCopyLocalHostName(SCDynamicStoreRef store)
{
	CFStringRef key, name = NULL;
	CFDictionaryRef dict;

	key = SCDynamicStoreKeyCreateHostNames(NULL);
	if (key == NULL)
		return (NULL);
	dict = (CFDictionaryRef)SCDynamicStoreCopyValue(store, key);
	CFRelease(key);
	if (dict == NULL)
		return (NULL);
	if (CFGetTypeID(dict) == CFDictionaryGetTypeID()) {
		CFStringRef v = CFDictionaryGetValue(dict,
		    kSCPropNetLocalHostName);
		if (v != NULL && CFGetTypeID(v) == CFStringGetTypeID())
			name = CFStringCreateCopy(NULL, v);
	}
	CFRelease(dict);
	return (name);
}

CFStringRef
SCDynamicStoreCopyComputerName(SCDynamicStoreRef store,
    CFStringEncoding *encoding)
{
	CFStringRef key, name = NULL;
	CFDictionaryRef dict;

	if (encoding != NULL)
		*encoding = kCFStringEncodingUTF8;

	key = SCDynamicStoreKeyCreateComputerName(NULL);
	if (key == NULL)
		return (NULL);
	dict = (CFDictionaryRef)SCDynamicStoreCopyValue(store, key);
	CFRelease(key);
	if (dict == NULL)
		return (NULL);
	if (CFGetTypeID(dict) == CFDictionaryGetTypeID()) {
		CFStringRef v = CFDictionaryGetValue(dict,
		    kSCPropSystemComputerName);
		if (v != NULL && CFGetTypeID(v) == CFStringGetTypeID())
			name = CFStringCreateCopy(NULL, v);
		if (encoding != NULL) {
			CFNumberRef enc = CFDictionaryGetValue(dict,
			    kSCPropSystemComputerNameEncoding);
			if (enc != NULL &&
			    CFGetTypeID(enc) == CFNumberGetTypeID()) {
				SInt32 v32;
				if (CFNumberGetValue(enc, kCFNumberSInt32Type,
				    &v32))
					*encoding = (CFStringEncoding)v32;
			}
		}
	}
	CFRelease(dict);
	return (name);
}
