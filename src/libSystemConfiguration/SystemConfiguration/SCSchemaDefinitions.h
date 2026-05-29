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
 * SCSchemaDefinitions.h — the canonical SCDynamicStore + SCPreferences
 * schema constant names. freebsd-launchd-mach port of Apple's
 * SystemConfiguration.framework SCSchemaDefinitions.h, populated as the
 * surface our daemons consume grows.
 *
 * Constants are CFSTR'd strings whose exact spelling is part of Apple's
 * stable ABI — every SC-aware daemon (configd, IPMonitor, mDNSResponder,
 * Setup Assistant, ...) reads/writes by these names. Do not edit values;
 * they are sync'd against
 * apple-oss-distributions/configd/SystemConfiguration.fproj/
 * SCSchemaDefinitions.h.
 *
 * Only the subset our in-tree consumers need is exported. Add more as
 * additional clients (e.g. mDNSResponder's reactive watcher, future
 * configd plugins) land.
 */

#ifndef _SCSCHEMADEFINITIONS_H
#define _SCSCHEMADEFINITIONS_H

#include <CoreFoundation/CoreFoundation.h>

/*
 * SCDynamicStore key-name domain prefixes. The store namespace is
 * partitioned into "State:/" (current runtime state, ephemeral) and
 * "Setup:/" (persistent configuration), with sub-paths for /Network/
 * /System/ etc. Apple's SCDynamicStoreKeyCreate* helpers stitch these
 * together with entity names.
 */
#define kSCDynamicStoreDomainState		CFSTR("State:")
#define kSCDynamicStoreDomainSetup		CFSTR("Setup:")
#define kSCDynamicStoreDomainFile		CFSTR("File:")
#define kSCDynamicStoreDomainPlugin		CFSTR("Plugin:")
#define kSCDynamicStoreDomainPrefs		CFSTR("Prefs:")

/*
 * Generic component names used by KeyCreate helpers. kSCCompAnyRegex
 * matches any UUID segment in a SCDynamicStore pattern subscription
 * (POSIX regex inside SCDynamicStoreSetNotificationKeys patterns).
 */
#define kSCCompNetwork				CFSTR("Network")
#define kSCCompService				CFSTR("Service")
#define kSCCompGlobal				CFSTR("Global")
#define kSCCompHostNames			CFSTR("HostNames")
#define kSCCompSystem				CFSTR("System")
#define kSCCompUsers				CFSTR("Users")
#define kSCCompInterface			CFSTR("Interface")
#define kSCCompAnyRegex				CFSTR("[^/]+")

/*
 * Network-entity names that appear as the trailing component of
 * State:/Network/Service/<UUID>/<entity> and analogous keys. Subset
 * we currently consume; populate as needed.
 */
#define kSCEntNetIPv4				CFSTR("IPv4")
#define kSCEntNetIPv6				CFSTR("IPv6")
#define kSCEntNetDHCP				CFSTR("DHCP")
#define kSCEntNetDHCPv6				CFSTR("DHCPv6")
#define kSCEntNetDNS				CFSTR("DNS")
#define kSCEntNetProxies			CFSTR("Proxies")
#define kSCEntNetInterface			CFSTR("Interface")
#define kSCEntNetLink				CFSTR("Link")

/*
 * Property names that appear as dictionary keys inside the network-
 * entity dicts published into the SCDynamicStore.
 */
#define kSCPropNetIPv4Addresses			CFSTR("Addresses")
#define kSCPropNetIPv4SubnetMasks		CFSTR("SubnetMasks")
#define kSCPropNetIPv4Router			CFSTR("Router")
#define kSCPropNetInterfaceDeviceName		CFSTR("DeviceName")
#define kSCPropNetInterfaceType			CFSTR("Type")
#define kSCPropNetInterfaceHardware		CFSTR("Hardware")
#define kSCPropUserDefinedName			CFSTR("UserDefinedName")
#define kSCPropSystemHostName			CFSTR("HostName")
#define kSCPropSystemComputerName		CFSTR("ComputerName")
#define kSCPropSystemComputerNameEncoding	CFSTR("ComputerNameEncoding")
#define kSCPropNetHostName			CFSTR("HostName")
#define kSCPropNetLocalHostName			CFSTR("LocalHostName")

/*
 * State:/Network/Global/IPv4 properties. PrimaryService is the
 * service UUID of the primary network service; PrimaryInterface is
 * its BSD interface name. ipconfigd publishes these on BOUND;
 * IPMonitor's set-hostname.c and mDNSResponder read them.
 */
#define kSCDynamicStorePropNetPrimaryService	CFSTR("PrimaryService")
#define kSCDynamicStorePropNetPrimaryInterface	CFSTR("PrimaryInterface")

#endif	/* _SCSCHEMADEFINITIONS_H */
