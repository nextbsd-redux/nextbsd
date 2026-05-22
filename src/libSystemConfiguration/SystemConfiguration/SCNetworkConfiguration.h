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
#include <SystemConfiguration/SCPreferences.h>

/*!
	@typedef SCNetworkInterfaceRef
	@discussion Handle to a network interface (e.g. "en0"). A
		CoreFoundation type — see SCNetworkInterfaceGetTypeID().
 */
typedef const struct __SCNetworkInterface *	SCNetworkInterfaceRef;

/*!
	@typedef SCBondInterfaceRef
	@typedef SCBridgeInterfaceRef
	@typedef SCVLANInterfaceRef
	@discussion The virtual network interfaces — link aggregation,
		bridging and 802.1Q VLANs — are each an SCNetworkInterface.
 */
typedef SCNetworkInterfaceRef			SCBondInterfaceRef;
typedef SCNetworkInterfaceRef			SCBridgeInterfaceRef;
typedef SCNetworkInterfaceRef			SCVLANInterfaceRef;

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

/* --------------------------------------------------------------------
 * SCNetworkProtocol
 * ------------------------------------------------------------------ */

/*!
	@function SCNetworkProtocolGetTypeID
	@discussion Returns the CoreFoundation type identifier of all
		SCNetworkProtocol instances.
 */
CFTypeID
SCNetworkProtocolGetTypeID		(void);

/*!
	@function SCNetworkProtocolGetProtocolType
	@discussion Returns the protocol type — one of the
		kSCNetworkProtocolType* constants. Owned by the protocol.
 */
CFStringRef
SCNetworkProtocolGetProtocolType	(SCNetworkProtocolRef		protocol);

/*!
	@function SCNetworkProtocolGetConfiguration
	@discussion Returns the protocol's configuration dictionary, or
		NULL if it has none. Owned by the protocol.
 */
CFDictionaryRef
SCNetworkProtocolGetConfiguration	(SCNetworkProtocolRef		protocol);

/*!
	@function SCNetworkProtocolSetConfiguration
	@discussion Stores the protocol's configuration dictionary. The
		protocol's enabled/disabled state is preserved.
 */
Boolean
SCNetworkProtocolSetConfiguration	(SCNetworkProtocolRef		protocol,
					 CFDictionaryRef		config);

/*!
	@function SCNetworkProtocolGetEnabled
	@discussion Returns whether the protocol is enabled.
 */
Boolean
SCNetworkProtocolGetEnabled		(SCNetworkProtocolRef		protocol);

/*!
	@function SCNetworkProtocolSetEnabled
	@discussion Enables or disables the protocol.
 */
Boolean
SCNetworkProtocolSetEnabled		(SCNetworkProtocolRef		protocol,
					 Boolean			enabled);

/* --------------------------------------------------------------------
 * SCNetworkService
 * ------------------------------------------------------------------ */

/*!
	@function SCNetworkServiceGetTypeID
	@discussion Returns the CoreFoundation type identifier of all
		SCNetworkService instances.
 */
CFTypeID
SCNetworkServiceGetTypeID		(void);

/*!
	@function SCNetworkServiceCreate
	@discussion Creates a new network service bound to `interface`,
		stored in `prefs`. Release the returned value.
 */
SCNetworkServiceRef
SCNetworkServiceCreate			(SCPreferencesRef		prefs,
					 SCNetworkInterfaceRef		interface);

/*!
	@function SCNetworkServiceCopy
	@discussion Returns the existing network service `serviceID` in
		`prefs`, or NULL. Release the returned value.
 */
SCNetworkServiceRef
SCNetworkServiceCopy			(SCPreferencesRef		prefs,
					 CFStringRef			serviceID);

/*!
	@function SCNetworkServiceCopyAll
	@discussion Returns every network service stored in `prefs`.
	@result a CFArray of SCNetworkServiceRef (caller releases).
 */
CFArrayRef
SCNetworkServiceCopyAll			(SCPreferencesRef		prefs);

/*!
	@function SCNetworkServiceGetServiceID
	@discussion Returns the service's identifier. Owned by the service.
 */
CFStringRef
SCNetworkServiceGetServiceID		(SCNetworkServiceRef		service);

/*!
	@function SCNetworkServiceGetName
	@discussion Returns the service's name — the stored name, or the
		associated interface's name if none has been set.
 */
CFStringRef
SCNetworkServiceGetName			(SCNetworkServiceRef		service);

/*!
	@function SCNetworkServiceSetName
	@discussion Sets the service's name.
 */
Boolean
SCNetworkServiceSetName			(SCNetworkServiceRef		service,
					 CFStringRef			name);

/*!
	@function SCNetworkServiceGetInterface
	@discussion Returns the interface the service is bound to. Owned
		by the service.
 */
SCNetworkInterfaceRef
SCNetworkServiceGetInterface		(SCNetworkServiceRef		service);

/*!
	@function SCNetworkServiceRemove
	@discussion Removes the service from the preferences.
 */
Boolean
SCNetworkServiceRemove			(SCNetworkServiceRef		service);

/*!
	@function SCNetworkServiceCopyProtocol
	@discussion Returns the service's configured protocol of type
		`protocolType`, or NULL. Release the returned value.
 */
SCNetworkProtocolRef
SCNetworkServiceCopyProtocol		(SCNetworkServiceRef		service,
					 CFStringRef			protocolType);

/*!
	@function SCNetworkServiceCopyProtocols
	@discussion Returns every protocol configured on the service.
	@result a CFArray of SCNetworkProtocolRef (caller releases).
 */
CFArrayRef
SCNetworkServiceCopyProtocols		(SCNetworkServiceRef		service);

/*!
	@function SCNetworkServiceAddProtocolType
	@discussion Adds an (empty) protocol of type `protocolType` to the
		service; configure it via SCNetworkProtocolSetConfiguration.
 */
Boolean
SCNetworkServiceAddProtocolType		(SCNetworkServiceRef		service,
					 CFStringRef			protocolType);

/*!
	@function SCNetworkServiceRemoveProtocolType
	@discussion Removes the protocol of type `protocolType` from the
		service.
 */
Boolean
SCNetworkServiceRemoveProtocolType	(SCNetworkServiceRef		service,
					 CFStringRef			protocolType);

/* --------------------------------------------------------------------
 * SCNetworkSet
 * ------------------------------------------------------------------ */

/*!
	@function SCNetworkSetGetTypeID
	@discussion Returns the CoreFoundation type identifier of all
		SCNetworkSet instances.
 */
CFTypeID
SCNetworkSetGetTypeID			(void);

/*!
	@function SCNetworkSetCreate
	@discussion Creates a new, empty network set in `prefs`. Release
		the returned value.
 */
SCNetworkSetRef
SCNetworkSetCreate			(SCPreferencesRef		prefs);

/*!
	@function SCNetworkSetCopy
	@discussion Returns the existing network set `setID` in `prefs`,
		or NULL. Release the returned value.
 */
SCNetworkSetRef
SCNetworkSetCopy			(SCPreferencesRef		prefs,
					 CFStringRef			setID);

/*!
	@function SCNetworkSetCopyAll
	@discussion Returns every network set stored in `prefs`.
	@result a CFArray of SCNetworkSetRef (caller releases).
 */
CFArrayRef
SCNetworkSetCopyAll			(SCPreferencesRef		prefs);

/*!
	@function SCNetworkSetCopyCurrent
	@discussion Returns the current (active) network set, or NULL.
		Release the returned value.
 */
SCNetworkSetRef
SCNetworkSetCopyCurrent			(SCPreferencesRef		prefs);

/*!
	@function SCNetworkSetSetCurrent
	@discussion Makes `set` the current (active) network set.
 */
Boolean
SCNetworkSetSetCurrent			(SCNetworkSetRef		set);

/*!
	@function SCNetworkSetGetSetID
	@discussion Returns the set's identifier. Owned by the set.
 */
CFStringRef
SCNetworkSetGetSetID			(SCNetworkSetRef		set);

/*!
	@function SCNetworkSetGetName
	@discussion Returns the set's name, or NULL if it has none.
 */
CFStringRef
SCNetworkSetGetName			(SCNetworkSetRef		set);

/*!
	@function SCNetworkSetSetName
	@discussion Sets the set's name.
 */
Boolean
SCNetworkSetSetName			(SCNetworkSetRef		set,
					 CFStringRef			name);

/*!
	@function SCNetworkSetRemove
	@discussion Removes the set from the preferences. The current
		(active) set may not be removed.
 */
Boolean
SCNetworkSetRemove			(SCNetworkSetRef		set);

/*!
	@function SCNetworkSetCopyServices
	@discussion Returns the services that are members of the set.
	@result a CFArray of SCNetworkServiceRef (caller releases).
 */
CFArrayRef
SCNetworkSetCopyServices		(SCNetworkSetRef		set);

/*!
	@function SCNetworkSetAddService
	@discussion Adds `service` to the set.
 */
Boolean
SCNetworkSetAddService			(SCNetworkSetRef		set,
					 SCNetworkServiceRef		service);

/*!
	@function SCNetworkSetRemoveService
	@discussion Removes `service` from the set.
 */
Boolean
SCNetworkSetRemoveService		(SCNetworkSetRef		set,
					 SCNetworkServiceRef		service);

/*!
	@function SCNetworkSetGetServiceOrder
	@discussion Returns the set's service order — a CFArray of
		service identifiers — or NULL. Owned by the set.
 */
CFArrayRef
SCNetworkSetGetServiceOrder		(SCNetworkSetRef		set);

/*!
	@function SCNetworkSetSetServiceOrder
	@discussion Sets the set's service order.
 */
Boolean
SCNetworkSetSetServiceOrder		(SCNetworkSetRef		set,
					 CFArrayRef			newOrder);

/* --------------------------------------------------------------------
 * SCVLANInterface — 802.1Q VLAN virtual interfaces
 * ------------------------------------------------------------------ */

/*!
	@function SCVLANInterfaceCopyAll
	@discussion Returns every VLAN interface stored in `prefs`.
	@result a CFArray of SCVLANInterfaceRef (caller releases).
 */
CFArrayRef
SCVLANInterfaceCopyAll			(SCPreferencesRef		prefs);

/*!
	@function SCVLANInterfaceCopyAvailablePhysicalInterfaces
	@discussion Returns the interfaces a VLAN can be created on.
	@result a CFArray of SCNetworkInterfaceRef (caller releases).
 */
CFArrayRef
SCVLANInterfaceCopyAvailablePhysicalInterfaces
					(void);

/*!
	@function SCVLANInterfaceCreate
	@discussion Creates a new VLAN interface on `physical` with 802.1Q
		tag `tag` (1..4094), stored in `prefs`. Release the result.
 */
SCVLANInterfaceRef
SCVLANInterfaceCreate			(SCPreferencesRef		prefs,
					 SCNetworkInterfaceRef		physical,
					 CFNumberRef			tag);

/*!
	@function SCVLANInterfaceRemove
	@discussion Removes the VLAN interface from the preferences.
 */
Boolean
SCVLANInterfaceRemove			(SCVLANInterfaceRef		vlan);

/*!
	@function SCVLANInterfaceGetPhysicalInterface
	@discussion Returns the VLAN's physical interface. Owned by the
		VLAN.
 */
SCNetworkInterfaceRef
SCVLANInterfaceGetPhysicalInterface	(SCVLANInterfaceRef		vlan);

/*!
	@function SCVLANInterfaceGetTag
	@discussion Returns the VLAN's 802.1Q tag. Owned by the VLAN.
 */
CFNumberRef
SCVLANInterfaceGetTag			(SCVLANInterfaceRef		vlan);

/*!
	@function SCVLANInterfaceGetOptions
	@discussion Returns the VLAN's options dictionary, or NULL.
 */
CFDictionaryRef
SCVLANInterfaceGetOptions		(SCVLANInterfaceRef		vlan);

/*!
	@function SCVLANInterfaceSetPhysicalInterfaceAndTag
	@discussion Changes the VLAN's physical interface and tag.
 */
Boolean
SCVLANInterfaceSetPhysicalInterfaceAndTag
					(SCVLANInterfaceRef		vlan,
					 SCNetworkInterfaceRef		physical,
					 CFNumberRef			tag);

/*!
	@function SCVLANInterfaceSetLocalizedDisplayName
	@discussion Sets the VLAN's display name.
 */
Boolean
SCVLANInterfaceSetLocalizedDisplayName	(SCVLANInterfaceRef		vlan,
					 CFStringRef			newName);

/*!
	@function SCVLANInterfaceSetOptions
	@discussion Sets the VLAN's options dictionary.
 */
Boolean
SCVLANInterfaceSetOptions		(SCVLANInterfaceRef		vlan,
					 CFDictionaryRef		newOptions);

/* --------------------------------------------------------------------
 * SCBondInterface — link-aggregation virtual interfaces
 * ------------------------------------------------------------------ */

/*!
	@function SCBondInterfaceCopyAll
	@discussion Returns every bond interface stored in `prefs`.
	@result a CFArray of SCBondInterfaceRef (caller releases).
 */
CFArrayRef
SCBondInterfaceCopyAll			(SCPreferencesRef		prefs);

/*!
	@function SCBondInterfaceCopyAvailableMemberInterfaces
	@discussion Returns the interfaces available to be bond members
		— hardware interfaces not already in a bond.
	@result a CFArray of SCNetworkInterfaceRef (caller releases).
 */
CFArrayRef
SCBondInterfaceCopyAvailableMemberInterfaces
					(SCPreferencesRef		prefs);

/*!
	@function SCBondInterfaceCreate
	@discussion Creates a new, empty bond interface in `prefs`.
		Release the returned value.
 */
SCBondInterfaceRef
SCBondInterfaceCreate			(SCPreferencesRef		prefs);

/*!
	@function SCBondInterfaceRemove
	@discussion Removes the bond interface from the preferences.
 */
Boolean
SCBondInterfaceRemove			(SCBondInterfaceRef		bond);

/*!
	@function SCBondInterfaceGetMemberInterfaces
	@discussion Returns the bond's member interfaces. Owned by the
		bond.
 */
CFArrayRef
SCBondInterfaceGetMemberInterfaces	(SCBondInterfaceRef		bond);

/*!
	@function SCBondInterfaceGetOptions
	@discussion Returns the bond's options dictionary, or NULL.
 */
CFDictionaryRef
SCBondInterfaceGetOptions		(SCBondInterfaceRef		bond);

/*!
	@function SCBondInterfaceSetMemberInterfaces
	@discussion Sets the bond's member interfaces.
 */
Boolean
SCBondInterfaceSetMemberInterfaces	(SCBondInterfaceRef		bond,
					 CFArrayRef			members);

/*!
	@function SCBondInterfaceSetLocalizedDisplayName
	@discussion Sets the bond's display name.
 */
Boolean
SCBondInterfaceSetLocalizedDisplayName	(SCBondInterfaceRef		bond,
					 CFStringRef			newName);

/*!
	@function SCBondInterfaceSetOptions
	@discussion Sets the bond's options dictionary.
 */
Boolean
SCBondInterfaceSetOptions		(SCBondInterfaceRef		bond,
					 CFDictionaryRef		newOptions);

__END_DECLS

#endif	/* _SCNETWORKCONFIGURATION_H */
