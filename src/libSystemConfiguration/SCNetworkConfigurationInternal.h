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
 * The SCNetworkInterface object. A CoreFoundation runtime type — the
 * leading CFRuntimeBase is filled in by _CFRuntimeCreateInstance(), and
 * CFRetain() / CFRelease() drive its lifetime.
 *
 * A leaf (hardware) interface is the common case: SCNetworkInterfaceCopy
 * All builds one per enumerated BSD interface. `interface` is non-NULL
 * only for a layered interface (PPP-over-modem, VLAN, ...) — deferred to
 * a later iteration. `prefs` / `serviceID` are set when the interface
 * is bound to a stored network service (the SCNetworkService iteration).
 */
typedef struct __SCNetworkInterface {
	CFRuntimeBase		cfBase;

	/* interface identity */
	CFStringRef		interface_type;		/* kSCNetworkInterfaceType* */
	CFStringRef		entity_device;		/* BSD name, e.g. "en0" */

	/* display names */
	CFStringRef		name;			/* non-localized */
	CFStringRef		localized_name;		/* localized */

	/* link-layer (hardware) address */
	CFDataRef		address;		/* raw bytes */
	CFStringRef		addressString;		/* "xx:xx:..." */
	Boolean			builtin;

	/* layering — non-NULL only for a layered interface */
	SCNetworkInterfaceRef	interface;

	/* preferences entity bookkeeping (the SCNetworkService iteration) */
	SCPreferencesRef	prefs;			/* bound prefs, or NULL */
	CFStringRef		serviceID;		/* bound service, or NULL */
	CFStringRef		entity_type;		/* prefs entity "Type" */
	CFStringRef		entity_subtype;		/* prefs entity "SubType" */

	/* what can be layered / configured on this interface */
	CFArrayRef		supported_interface_types;
	CFArrayRef		supported_protocol_types;
} SCNetworkInterfacePrivate, *SCNetworkInterfacePrivateRef;

/*
 * SCNetworkInterface.c — allocate a bare SCNetworkInterface (every field
 * NULL / FALSE). Returns a retained instance, or NULL with SCError() set.
 */
SCNetworkInterfacePrivateRef
__SCNetworkInterfaceCreatePrivate	(CFAllocatorRef			allocator);

#endif	/* _SCNETWORKCONFIGURATIONINTERNAL_H */
