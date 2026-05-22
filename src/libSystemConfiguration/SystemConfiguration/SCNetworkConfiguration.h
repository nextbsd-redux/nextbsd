/*
 * Copyright (c) 2004-2022 Apple Inc. All rights reserved.
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
 * SCNetworkConfiguration.h — the SCNetworkConfiguration API.
 *
 * freebsd-launchd-mach port: a trimmed port of Apple's
 * SystemConfiguration.framework SCNetworkConfiguration.h. The
 * SCNetworkConfiguration API is the CoreFoundation-typed object model
 * for the persistent network configuration — interfaces, the protocols
 * (IPv4, IPv6, DNS, Proxies) layered on them, the services that bind an
 * interface to its protocols, and the sets ("locations") that group
 * services. Everything but SCNetworkInterface is persisted through the
 * SCPreferences path accessors.
 *
 * The port lands one object family per iteration:
 *   iter 2 — SCNetworkInterface (this is what is declared so far);
 *   iter 3 — SCNetworkProtocol + SCNetworkService;
 *   iter 4 — SCNetworkSet;
 *   iter 5 — the virtual (Bond / Bridge / VLAN) interfaces.
 * The SCNetworkService / Protocol / Set function declarations are added
 * to this header as those iterations land.
 */

#ifndef _SCNETWORKCONFIGURATION_H
#define _SCNETWORKCONFIGURATION_H

#include <sys/cdefs.h>
#include <CoreFoundation/CoreFoundation.h>

/*!
	@typedef SCNetworkInterfaceRef
	@discussion Handle to a network interface (e.g. "en0"). A
		CoreFoundation type — see SCNetworkInterfaceGetTypeID().
 */
typedef const struct __SCNetworkInterface *	SCNetworkInterfaceRef;

/*!
	@typedef SCNetworkProtocolRef
	@discussion Handle to the configuration of one protocol (IPv4,
		IPv6, DNS, Proxies, SMB) on a network service.
 */
typedef const struct __SCNetworkProtocol *	SCNetworkProtocolRef;

/*!
	@typedef SCNetworkServiceRef
	@discussion Handle to a network service — an interface together
		with the protocols configured on it.
 */
typedef const struct __SCNetworkService *	SCNetworkServiceRef;

/*!
	@typedef SCNetworkSetRef
	@discussion Handle to a network set ("location") — an ordered
		collection of network services.
 */
typedef const struct __SCNetworkSet *		SCNetworkSetRef;

/*
 * Network interface types. The string values are the ones stored in the
 * preferences plist, so they must match Apple's exactly.
 */
extern const CFStringRef kSCNetworkInterfaceType6to4;
extern const CFStringRef kSCNetworkInterfaceTypeBluetooth;
extern const CFStringRef kSCNetworkInterfaceTypeBond;
extern const CFStringRef kSCNetworkInterfaceTypeBridge;
extern const CFStringRef kSCNetworkInterfaceTypeEthernet;
extern const CFStringRef kSCNetworkInterfaceTypeFireWire;
extern const CFStringRef kSCNetworkInterfaceTypeIEEE80211;	/* Wi-Fi */
extern const CFStringRef kSCNetworkInterfaceTypeIPSec;
extern const CFStringRef kSCNetworkInterfaceTypeL2TP;
extern const CFStringRef kSCNetworkInterfaceTypeLoopback;
extern const CFStringRef kSCNetworkInterfaceTypeModem;
extern const CFStringRef kSCNetworkInterfaceTypePPP;
extern const CFStringRef kSCNetworkInterfaceTypeSerial;
extern const CFStringRef kSCNetworkInterfaceTypeVLAN;
extern const CFStringRef kSCNetworkInterfaceTypeWWAN;
extern const CFStringRef kSCNetworkInterfaceTypeIPv4;

/*
 * Network protocol types — the protocols that can be layered on a
 * service. Also the preferences-plist entity names.
 */
extern const CFStringRef kSCNetworkProtocolTypeDNS;
extern const CFStringRef kSCNetworkProtocolTypeIPv4;
extern const CFStringRef kSCNetworkProtocolTypeIPv6;
extern const CFStringRef kSCNetworkProtocolTypeProxies;
extern const CFStringRef kSCNetworkProtocolTypeSMB;

__BEGIN_DECLS

/* --------------------------------------------------------------------
 * SCNetworkInterface
 * ------------------------------------------------------------------ */

/*!
	@function SCNetworkInterfaceGetTypeID
	@discussion Returns the CoreFoundation type identifier of all
		SCNetworkInterface instances.
 */
CFTypeID
SCNetworkInterfaceGetTypeID		(void);

/*!
	@function SCNetworkInterfaceCopyAll
	@discussion Returns every network-capable interface on the system.
	@result a CFArray of SCNetworkInterfaceRef (caller releases).
 */
CFArrayRef
SCNetworkInterfaceCopyAll		(void);

/*!
	@function SCNetworkInterfaceGetBSDName
	@discussion Returns the BSD name (e.g. "en0") of the interface, or
		NULL if it has none. Owned by the interface.
 */
CFStringRef
SCNetworkInterfaceGetBSDName		(SCNetworkInterfaceRef		interface);

/*!
	@function SCNetworkInterfaceGetInterfaceType
	@discussion Returns the interface type — one of the
		kSCNetworkInterfaceType* constants. Owned by the interface.
 */
CFStringRef
SCNetworkInterfaceGetInterfaceType	(SCNetworkInterfaceRef		interface);

/*!
	@function SCNetworkInterfaceGetHardwareAddressString
	@discussion Returns the link-layer (MAC) address as a displayable
		"xx:xx:xx:xx:xx:xx" string, or NULL. Owned by the interface.
 */
CFStringRef
SCNetworkInterfaceGetHardwareAddressString
					(SCNetworkInterfaceRef		interface);

/*!
	@function SCNetworkInterfaceGetLocalizedDisplayName
	@discussion Returns a human-readable name (e.g. "Ethernet",
		"Wi-Fi") for the interface, or NULL. Owned by the interface.
 */
CFStringRef
SCNetworkInterfaceGetLocalizedDisplayName
					(SCNetworkInterfaceRef		interface);

/*!
	@function SCNetworkInterfaceGetInterface
	@discussion For a layered interface, returns the interface it is
		built on; NULL for a leaf (hardware) interface.
 */
SCNetworkInterfaceRef
SCNetworkInterfaceGetInterface		(SCNetworkInterfaceRef		interface);

/*!
	@function SCNetworkInterfaceGetSupportedInterfaceTypes
	@discussion Returns the interface types that can be layered on top
		of this interface, or NULL if none. Owned by the interface.
 */
CFArrayRef
SCNetworkInterfaceGetSupportedInterfaceTypes
					(SCNetworkInterfaceRef		interface);

/*!
	@function SCNetworkInterfaceGetSupportedProtocolTypes
	@discussion Returns the protocol types that can be configured on
		this interface, or NULL if none. Owned by the interface.
 */
CFArrayRef
SCNetworkInterfaceGetSupportedProtocolTypes
					(SCNetworkInterfaceRef		interface);

__END_DECLS

#endif	/* _SCNETWORKCONFIGURATION_H */
