/*
 * SCNetworkConfigurationInternal.h — private definitions shared by the
 * SCNetworkConfiguration sources (freebsd-launchd-mach port of Apple's
 * SystemConfiguration.fproj SCNetworkConfigurationInternal.h).
 *
 * Not installed.
 */

#ifndef _SCNETWORKCONFIGURATIONINTERNAL_H
#define _SCNETWORKCONFIGURATIONINTERNAL_H

#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFRuntime.h>

#include <SystemConfiguration/SCNetworkConfiguration.h>
#include <SystemConfiguration/SCPreferences.h>

/*
 * Reserved preferences-plist keys. The network configuration lives in
 * the preferences plist under /NetworkServices/<serviceID>, each
 * service dict carrying an "Interface" entity, a "UserDefinedName", and
 * one sub-dict per configured protocol. A "__INACTIVE__" key marks a
 * service or protocol disabled.
 */
#define kSCResvInactive			CFSTR("__INACTIVE__")
#define kSCPrefNetworkServices		CFSTR("NetworkServices")
#define kSCPrefSets			CFSTR("Sets")
#define kSCPrefCurrentSet		CFSTR("CurrentSet")
#define kSCCompNetwork			CFSTR("Network")
#define kSCCompService			CFSTR("Service")
#define kSCCompGlobal			CFSTR("Global")
#define kSCEntNetInterface		CFSTR("Interface")
#define kSCEntNetIPv4			CFSTR("IPv4")
#define kSCPropUserDefinedName		CFSTR("UserDefinedName")
#define kSCPropNetServiceOrder		CFSTR("ServiceOrder")
#define kSCPropNetInterfaceType		CFSTR("Type")
#define kSCPropNetInterfaceDeviceName	CFSTR("DeviceName")
#define kSCPropNetInterfaceHardware	CFSTR("Hardware")


#pragma mark -
#pragma mark SCNetworkInterface

/*
 * The SCNetworkInterface object — see SCNetworkInterface.c. A leaf
 * (hardware) interface is the common case; `interface` is non-NULL only
 * for a layered interface. `prefs` / `serviceID` are set when the
 * interface is bound to a stored network service.
 */
typedef struct __SCNetworkInterface {
	CFRuntimeBase		cfBase;

	CFStringRef		interface_type;		/* kSCNetworkInterfaceType* */
	CFStringRef		entity_device;		/* BSD name, e.g. "en0" */

	CFStringRef		name;			/* non-localized */
	CFStringRef		localized_name;		/* localized */

	CFDataRef		address;		/* raw link-layer bytes */
	CFStringRef		addressString;		/* "xx:xx:..." */
	Boolean			builtin;

	SCNetworkInterfaceRef	interface;		/* layered-on, or NULL */

	SCPreferencesRef	prefs;			/* bound prefs, or NULL */
	CFStringRef		serviceID;		/* bound service, or NULL */
	CFStringRef		entity_type;		/* prefs entity "Type" */
	CFStringRef		entity_subtype;		/* prefs entity "SubType" */

	CFArrayRef		supported_interface_types;
	CFArrayRef		supported_protocol_types;
} SCNetworkInterfacePrivate, *SCNetworkInterfacePrivateRef;


#pragma mark -
#pragma mark SCNetworkService

/*
 * The SCNetworkService object — see SCNetworkService.c. A network
 * service binds an interface to the protocols configured on it; it is
 * persisted at /NetworkServices/<serviceID> in the preferences plist.
 */
typedef struct __SCNetworkService {
	CFRuntimeBase		cfBase;

	CFStringRef		serviceID;
	SCNetworkInterfaceRef	interface;	/* lazily reconstructed */
	SCPreferencesRef	prefs;
	CFStringRef		name;		/* cached UserDefinedName */
} SCNetworkServicePrivate, *SCNetworkServicePrivateRef;


#pragma mark -
#pragma mark SCNetworkProtocol

/*
 * The SCNetworkProtocol object — see SCNetworkProtocol.c. One protocol
 * (IPv4, IPv6, DNS, Proxies, SMB) configured on a network service;
 * persisted at /NetworkServices/<serviceID>/<entityID>.
 */
typedef struct __SCNetworkProtocol {
	CFRuntimeBase		cfBase;

	CFStringRef		entityID;	/* the protocol type */
	SCNetworkServiceRef	service;
} SCNetworkProtocolPrivate, *SCNetworkProtocolPrivateRef;


#pragma mark -
#pragma mark SCNetworkSet

/*
 * The SCNetworkSet object — see SCNetworkSet.c. A set ("location") is an
 * ordered collection of network services; persisted at /Sets/<setID> in
 * the preferences plist, with the active set named by the top-level
 * "CurrentSet" key.
 */
typedef struct __SCNetworkSet {
	CFRuntimeBase		cfBase;

	CFStringRef		setID;
	SCPreferencesRef	prefs;
	CFStringRef		name;		/* cached UserDefinedName */
} SCNetworkSetPrivate, *SCNetworkSetPrivateRef;


#pragma mark -
#pragma mark Type checks

static inline Boolean
isA_SCNetworkInterface(SCNetworkInterfaceRef interface)
{
	return ((interface != NULL) &&
		(CFGetTypeID(interface) == SCNetworkInterfaceGetTypeID()));
}

static inline Boolean
isA_SCNetworkService(SCNetworkServiceRef service)
{
	return ((service != NULL) &&
		(CFGetTypeID(service) == SCNetworkServiceGetTypeID()));
}

static inline Boolean
isA_SCNetworkProtocol(SCNetworkProtocolRef protocol)
{
	return ((protocol != NULL) &&
		(CFGetTypeID(protocol) == SCNetworkProtocolGetTypeID()));
}

static inline Boolean
isA_SCNetworkSet(SCNetworkSetRef set)
{
	return ((set != NULL) &&
		(CFGetTypeID(set) == SCNetworkSetGetTypeID()));
}


#pragma mark -
#pragma mark Internal helpers

/* SCNetworkInterface.c — allocate a bare interface (all fields NULL). */
SCNetworkInterfacePrivateRef
__SCNetworkInterfaceCreatePrivate	(CFAllocatorRef		allocator);

/*
 * SCNetworkInterface.c — the interface <-> preferences-entity bridge.
 * __SCNetworkInterfaceCopyInterfaceEntity serializes an interface into
 * the dictionary stored at a service's "Interface" path; _SCNetworkInter
 * faceCreateWithEntity rebuilds an interface from that dictionary.
 */
CFDictionaryRef
__SCNetworkInterfaceCopyInterfaceEntity	(SCNetworkInterfaceRef	interface);

SCNetworkInterfaceRef
_SCNetworkInterfaceCreateWithEntity	(CFAllocatorRef		allocator,
					 CFDictionaryRef	entity,
					 SCNetworkServiceRef	service);

/* SCNetworkService.c — allocate a service object. */
SCNetworkServicePrivateRef
__SCNetworkServiceCreatePrivate		(CFAllocatorRef		allocator,
					 SCPreferencesRef	prefs,
					 CFStringRef		serviceID,
					 SCNetworkInterfaceRef	interface);

/* SCNetworkProtocol.c — allocate a protocol object. */
SCNetworkProtocolPrivateRef
__SCNetworkProtocolCreatePrivate	(CFAllocatorRef		allocator,
					 CFStringRef		entityID,
					 SCNetworkServiceRef	service);

/* SCNetworkSet.c — allocate a set object. */
SCNetworkSetPrivateRef
__SCNetworkSetCreatePrivate		(CFAllocatorRef		allocator,
					 SCPreferencesRef	prefs,
					 CFStringRef		setID);

/* SCNetworkProtocol.c — TRUE iff `protocolType` is a known protocol. */
Boolean
__SCNetworkProtocolIsValidType		(CFStringRef		protocolType);

/*
 * SCNetworkConfigurationInternal.c — build a preferences path for a
 * network service or one of its entities: entity NULL gives
 * "/NetworkServices/<serviceID>", otherwise
 * "/NetworkServices/<serviceID>/<entity>". Caller releases.
 */
CFStringRef
__SCNetworkServiceEntityPath		(CFStringRef		serviceID,
					 CFStringRef		entity);

#endif	/* _SCNETWORKCONFIGURATIONINTERNAL_H */
