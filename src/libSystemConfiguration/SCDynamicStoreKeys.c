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
 * SCDynamicStoreKeys.c — the canonical SCDynamicStoreKeyCreate* helpers
 * that stitch domain prefixes + component names + entity names into
 * the SCDynamicStore key strings that every Apple-shape daemon reads
 * and writes.
 *
 * freebsd-launchd-mach port: small. Apple's reference lives at
 * apple-oss-distributions/configd/SystemConfiguration.fproj/
 * SCDynamicStoreKey.c — we mirror the contract (every helper returns
 * a fresh CFStringRef the caller releases) without dragging in the
 * full Apple file, which has macOS-specific bonjour / proxies / user-
 * session keys we do not currently consume.
 *
 * Used by:
 *   - hostnamed (vendored Apple set-hostname.c) for reading the
 *     Setup:/System ComputerName and the State:/Network/Service/.../
 *     {DHCP,IPv4} entities.
 *   - mDNSResponder (when its #62 reactive watcher subscribes to the
 *     same keys).
 *   - ipconfigd (publishes Service/<UUID>/{IPv4,DHCP} + Global/IPv4
 *     via these same paths).
 */

#include <SystemConfiguration/SCDynamicStore.h>
#include <SystemConfiguration/SCSchemaDefinitions.h>

#include <CoreFoundation/CoreFoundation.h>

/* Shared formatter — every helper builds "<domain>/<components.../>". */
static CFStringRef
make_key(CFAllocatorRef allocator, CFStringRef format, ...)
{
	CFStringRef key;
	va_list ap;

	va_start(ap, format);
	key = CFStringCreateWithFormatAndArguments(allocator, NULL, format,
	    ap);
	va_end(ap);
	return (key);
}

/*
 * SCDynamicStoreKeyCreateComputerName — "Setup:/System".
 * The Setup:/System dict carries ComputerName + ComputerNameEncoding,
 * the user-visible "My Mac"-style name. PreferencesMonitor (or our
 * prefs_monitor equivalent) writes this from /Library/Preferences/
 * SystemConfiguration/preferences.plist on commit.
 */
CFStringRef
SCDynamicStoreKeyCreateComputerName(CFAllocatorRef allocator)
{
	return (make_key(allocator, CFSTR("%@/%@"),
	    kSCDynamicStoreDomainSetup, kSCCompSystem));
}

/*
 * SCDynamicStoreKeyCreateHostNames — "Setup:/Network/HostNames".
 * The dict carries HostName (DNS-canonical) + LocalHostName (Bonjour
 * label). mDNSResponder owns LocalHostName via conflict-rename;
 * everything else reads.
 */
CFStringRef
SCDynamicStoreKeyCreateHostNames(CFAllocatorRef allocator)
{
	return (make_key(allocator, CFSTR("%@/%@/%@"),
	    kSCDynamicStoreDomainSetup, kSCCompNetwork, kSCCompHostNames));
}

/*
 * SCDynamicStoreKeyCreateNetworkGlobalEntity — "<domain>:/Network/
 * Global/<entity>". The Global/IPv4 dict carries PrimaryService /
 * PrimaryInterface; published by ipconfigd on BOUND.
 */
CFStringRef
SCDynamicStoreKeyCreateNetworkGlobalEntity(CFAllocatorRef allocator,
    CFStringRef domain, CFStringRef entity)
{
	return (make_key(allocator, CFSTR("%@/%@/%@/%@"),
	    domain, kSCCompNetwork, kSCCompGlobal, entity));
}

/*
 * SCDynamicStoreKeyCreateNetworkServiceEntity — "<domain>:/Network/
 * Service/<serviceID>/<entity>". serviceID is a UUID string; entity
 * may be the kSCCompAnyRegex sentinel for pattern subscriptions.
 * Apple's set-hostname.c uses both forms — direct lookup with a
 * concrete serviceID, and pattern subscription with the regex.
 */
CFStringRef
SCDynamicStoreKeyCreateNetworkServiceEntity(CFAllocatorRef allocator,
    CFStringRef domain, CFStringRef serviceID, CFStringRef entity)
{
	if (entity == NULL) {
		return (make_key(allocator, CFSTR("%@/%@/%@/%@"),
		    domain, kSCCompNetwork, kSCCompService, serviceID));
	}
	return (make_key(allocator, CFSTR("%@/%@/%@/%@/%@"),
	    domain, kSCCompNetwork, kSCCompService, serviceID, entity));
}

/*
 * SCDynamicStoreKeyCreateNetworkInterfaceEntity — analogous, keyed by
 * the BSD interface name (en0, vtnet0, ...). Less commonly used than
 * the service-entity form; included for completeness because Apple's
 * mDNSResponder reactive code may consume it.
 */
CFStringRef
SCDynamicStoreKeyCreateNetworkInterfaceEntity(CFAllocatorRef allocator,
    CFStringRef domain, CFStringRef ifname, CFStringRef entity)
{
	if (entity == NULL) {
		return (make_key(allocator, CFSTR("%@/%@/%@/%@"),
		    domain, kSCCompNetwork, kSCCompInterface, ifname));
	}
	return (make_key(allocator, CFSTR("%@/%@/%@/%@/%@"),
	    domain, kSCCompNetwork, kSCCompInterface, ifname, entity));
}
