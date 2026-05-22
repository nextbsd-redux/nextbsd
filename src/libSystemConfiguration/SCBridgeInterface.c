/*
 * SCBridgeInterface.c — the SCBridgeInterface API (freebsd-launchd-mach
 * port of Apple's SystemConfiguration.fproj BridgeConfiguration.c).
 *
 * A bridge interface joins several interfaces at layer 2 (FreeBSD
 * if_bridge(4)). It is an SCNetworkInterface of type Bridge
 * (SCBridgeInterfaceRef aliases SCNetworkInterfaceRef). Bridges are
 * persisted in the preferences plist at
 * /VirtualNetworkInterfaces/Bridge/<bridgeName>, each entry recording
 * the member interfaces' BSD names, a display name and options. The
 * "AllowConfiguredMembers" flag — whether a bridge member may itself
 * carry a configured network service — is kept in the options.
 *
 * Apple's BridgeConfiguration.c also reflects the live kernel bridge
 * state; the port keeps the stored configuration model.
 */

#include "SCNetworkConfigurationInternal.h"
#include "SCInternal.h"

#include <stdlib.h>

#define	kAllowConfiguredMembers		CFSTR("AllowConfiguredMembers")


#pragma mark -
#pragma mark Helpers

/* TRUE iff `bridge` is a live SCNetworkInterface of type Bridge */
static Boolean
isA_SCBridgeInterface(SCBridgeInterfaceRef bridge)
{
	return (isA_SCNetworkInterface(bridge) &&
		CFEqual(((SCNetworkInterfacePrivateRef)bridge)->interface_type,
			kSCNetworkInterfaceTypeBridge));
}

/* "/VirtualNetworkInterfaces/Bridge" */
static CFStringRef
__bridgeContainerPath(void)
{
	return CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@/%@"),
					kSCPrefVirtualNetworkInterfaces,
					kSCNetworkInterfaceTypeBridge);
}

/* "/VirtualNetworkInterfaces/Bridge/<bridgeName>" */
static CFStringRef
__bridgePath(CFStringRef bridgeName)
{
	return CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@/%@/%@"),
					kSCPrefVirtualNetworkInterfaces,
					kSCNetworkInterfaceTypeBridge,
					bridgeName);
}

/* allocate a Bridge interface object named `bridge_if` (e.g. "bridge0") */
static SCBridgeInterfaceRef
_SCBridgeInterfaceCreatePrivate(CFAllocatorRef allocator,
				CFStringRef bridge_if)
{
	SCNetworkInterfacePrivateRef	ip;

	ip = __SCNetworkInterfaceCreatePrivate(allocator);
	if (ip == NULL) {
		return NULL;
	}
	ip->interface_type =
		(CFStringRef)CFRetain(kSCNetworkInterfaceTypeBridge);
	ip->entity_device  = CFStringCreateCopy(allocator, bridge_if);
	ip->builtin        = TRUE;
	ip->localized_name = CFStringCreateWithCString(NULL, "Bridge",
						       kCFStringEncodingUTF8);
	ip->name           = (CFStringRef)CFRetain(ip->localized_name);
	/* a bridge carries the usual protocols, so a service can use it */
	ip->supported_protocol_types = __SCNetworkInterfaceCopyProtocolTypes();
	/* a fresh bridge has an (empty) member list, never NULL */
	ip->member_interfaces =
		CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);
	return (SCBridgeInterfaceRef)ip;
}

/*
 * Rewrite the stored entry for `bridge` with the value at `key` set to
 * `value` (NULL removes). A bridge with no prefs is in-memory only,
 * which the callers treat as success.
 */
static Boolean
__bridgeStoreSet(SCNetworkInterfacePrivateRef ip, CFStringRef key,
		 CFTypeRef value)
{
	CFDictionaryRef		dict;
	CFMutableDictionaryRef	newDict;
	CFStringRef		path;
	Boolean			ok;

	if (ip->prefs == NULL) {
		return TRUE;
	}
	path = __bridgePath(ip->entity_device);
	dict = SCPreferencesPathGetValue(ip->prefs, path);
	if ((dict == NULL) ||
	    (CFGetTypeID(dict) != CFDictionaryGetTypeID())) {
		CFRelease(path);
		_SCErrorSet(kSCStatusFailed);
		return FALSE;
	}
	newDict = CFDictionaryCreateMutableCopy(NULL, 0, dict);
	if (value != NULL) {
		CFDictionarySetValue(newDict, key, value);
	} else {
		CFDictionaryRemoveValue(newDict, key);
	}
	ok = SCPreferencesPathSetValue(ip->prefs, path, newDict);
	CFRelease(newDict);
	CFRelease(path);
	return ok;
}


#pragma mark -
#pragma mark SCBridgeInterface APIs

CFArrayRef
SCBridgeInterfaceCopyAll(SCPreferencesRef prefs)
{
	CFMutableArrayRef	array;
	CFDictionaryRef		container;
	const void		**keys;
	const void		**vals;
	CFIndex			i, n;
	CFStringRef		path;

	array = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

	path      = __bridgeContainerPath();
	container = SCPreferencesPathGetValue(prefs, path);
	CFRelease(path);
	if ((container == NULL) ||
	    (CFGetTypeID(container) != CFDictionaryGetTypeID())) {
		return array;
	}

	n = CFDictionaryGetCount(container);
	if (n <= 0) {
		return array;
	}
	keys = (const void **)malloc((size_t)n * sizeof(void *));
	vals = (const void **)malloc((size_t)n * sizeof(void *));
	CFDictionaryGetKeysAndValues(container, keys, vals);

	for (i = 0; i < n; i++) {
		SCNetworkInterfacePrivateRef	ip;
		SCBridgeInterfaceRef		bridge;
		CFDictionaryRef			info	= (CFDictionaryRef)vals[i];
		CFArrayRef			members;
		CFDictionaryRef			options;
		CFStringRef			name;
		CFMutableArrayRef		ifaces;
		CFIndex				j, m;

		if (CFGetTypeID(info) != CFDictionaryGetTypeID()) {
			continue;
		}
		bridge = _SCBridgeInterfaceCreatePrivate(NULL,
							 (CFStringRef)keys[i]);
		if (bridge == NULL) {
			continue;
		}
		ip        = (SCNetworkInterfacePrivateRef)bridge;
		ip->prefs = (SCPreferencesRef)CFRetain(prefs);

		/* rebuild each member from its stored BSD name */
		members = CFDictionaryGetValue(info, kSCPropVirtualInterfaces);
		m       = ((members != NULL) &&
			   (CFGetTypeID(members) == CFArrayGetTypeID()))
			  ? CFArrayGetCount(members) : 0;
		ifaces  = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
		for (j = 0; j < m; j++) {
			CFStringRef		bsd;
			SCNetworkInterfaceRef	iface;

			bsd = CFArrayGetValueAtIndex(members, j);
			if (CFGetTypeID(bsd) != CFStringGetTypeID()) {
				continue;
			}
			iface = __SCNetworkInterfaceCreateWithBSDName(NULL, bsd);
			if (iface != NULL) {
				CFArrayAppendValue(ifaces, iface);
				CFRelease(iface);
			}
		}
		CFRelease(ip->member_interfaces);
		ip->member_interfaces = ifaces;

		options = CFDictionaryGetValue(info, kSCPropVirtualOptions);
		if ((options != NULL) &&
		    (CFGetTypeID(options) == CFDictionaryGetTypeID())) {
			ip->virtual_options =
				CFDictionaryCreateCopy(NULL, options);
		}
		name = CFDictionaryGetValue(info, kSCPropUserDefinedName);
		if ((name != NULL) &&
		    (CFGetTypeID(name) == CFStringGetTypeID())) {
			if (ip->localized_name != NULL) {
				CFRelease(ip->localized_name);
			}
			ip->localized_name = CFStringCreateCopy(NULL, name);
		}

		CFArrayAppendValue(array, bridge);
		CFRelease(bridge);
	}

	free(keys);
	free(vals);
	return array;
}

CFArrayRef
SCBridgeInterfaceCopyAvailableMemberInterfaces(SCPreferencesRef prefs)
{
	CFMutableArrayRef	available;
	CFArrayRef		all;
	CFArrayRef		bridges;
	CFMutableSetRef		excluded;
	CFIndex			i, n;

	available = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	excluded  = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);

	/* an interface already in a bridge is not available */
	bridges = SCBridgeInterfaceCopyAll(prefs);
	n       = (bridges != NULL) ? CFArrayGetCount(bridges) : 0;
	for (i = 0; i < n; i++) {
		CFArrayRef	members;
		CFIndex		j, m;

		members = SCBridgeInterfaceGetMemberInterfaces(
				(SCBridgeInterfaceRef)
				CFArrayGetValueAtIndex(bridges, i));
		m = (members != NULL) ? CFArrayGetCount(members) : 0;
		for (j = 0; j < m; j++) {
			CFStringRef	bsd;

			bsd = SCNetworkInterfaceGetBSDName(
				(SCNetworkInterfaceRef)
				CFArrayGetValueAtIndex(members, j));
			if (bsd != NULL) {
				CFSetAddValue(excluded, bsd);
			}
		}
	}
	if (bridges != NULL) {
		CFRelease(bridges);
	}

	all = SCNetworkInterfaceCopyAll();
	n   = (all != NULL) ? CFArrayGetCount(all) : 0;
	for (i = 0; i < n; i++) {
		SCNetworkInterfaceRef	iface;
		CFStringRef		bsd;

		iface = (SCNetworkInterfaceRef)CFArrayGetValueAtIndex(all, i);
		bsd   = SCNetworkInterfaceGetBSDName(iface);
		if ((bsd != NULL) && !CFSetContainsValue(excluded, bsd)) {
			CFArrayAppendValue(available, iface);
		}
	}
	if (all != NULL) {
		CFRelease(all);
	}
	CFRelease(excluded);
	return available;
}

SCBridgeInterfaceRef
SCBridgeInterfaceCreate(SCPreferencesRef prefs)
{
	SCBridgeInterfaceRef	bridge	= NULL;
	int			i;

	if (prefs == NULL) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	/* claim the first free bridgeN name */
	for (i = 0; i < 1024; i++) {
		CFMutableDictionaryRef	entry;
		CFArrayRef		empty;
		CFStringRef		bridge_if;
		CFStringRef		path;
		Boolean			ok;

		bridge_if = CFStringCreateWithFormat(NULL, NULL,
						     CFSTR("bridge%d"), i);
		path      = __bridgePath(bridge_if);
		if (SCPreferencesPathGetValue(prefs, path) != NULL) {
			CFRelease(path);
			CFRelease(bridge_if);
			continue;
		}

		/* a new bridge starts with an empty member list */
		entry = CFDictionaryCreateMutable(NULL, 0,
					&kCFTypeDictionaryKeyCallBacks,
					&kCFTypeDictionaryValueCallBacks);
		empty = CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);
		CFDictionarySetValue(entry, kSCPropVirtualInterfaces, empty);
		CFRelease(empty);
		ok = SCPreferencesPathSetValue(prefs, path, entry);
		CFRelease(entry);
		CFRelease(path);
		if (!ok) {
			CFRelease(bridge_if);
			_SCErrorSet(kSCStatusFailed);
			return NULL;
		}

		bridge = _SCBridgeInterfaceCreatePrivate(NULL, bridge_if);
		CFRelease(bridge_if);
		if (bridge != NULL) {
			((SCNetworkInterfacePrivateRef)bridge)->prefs =
				(SCPreferencesRef)CFRetain(prefs);
		}
		break;
	}
	if (bridge == NULL) {
		_SCErrorSet(kSCStatusFailed);
	}
	return bridge;
}

Boolean
SCBridgeInterfaceRemove(SCBridgeInterfaceRef bridge)
{
	SCNetworkInterfacePrivateRef	ip	= (SCNetworkInterfacePrivateRef)bridge;
	CFStringRef			path;
	Boolean				ok;

	if (!isA_SCBridgeInterface(bridge) || (ip->prefs == NULL)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	path = __bridgePath(ip->entity_device);
	ok   = SCPreferencesPathRemoveValue(ip->prefs, path);
	CFRelease(path);
	return ok;
}

CFArrayRef
SCBridgeInterfaceGetMemberInterfaces(SCBridgeInterfaceRef bridge)
{
	if (!isA_SCBridgeInterface(bridge)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	return ((SCNetworkInterfacePrivateRef)bridge)->member_interfaces;
}

CFDictionaryRef
SCBridgeInterfaceGetOptions(SCBridgeInterfaceRef bridge)
{
	if (!isA_SCBridgeInterface(bridge)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	return ((SCNetworkInterfacePrivateRef)bridge)->virtual_options;
}

Boolean
SCBridgeInterfaceSetMemberInterfaces(SCBridgeInterfaceRef bridge,
				     CFArrayRef members)
{
	SCNetworkInterfacePrivateRef	ip	= (SCNetworkInterfacePrivateRef)bridge;
	CFMutableArrayRef		names;
	CFMutableArrayRef		ifaces;
	CFIndex				i, n;

	if (!isA_SCBridgeInterface(bridge)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if ((members != NULL) &&
	    (CFGetTypeID(members) != CFArrayGetTypeID())) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	/* the stored form is an array of member BSD names; the in-memory
	 * form keeps the interface objects */
	n      = (members != NULL) ? CFArrayGetCount(members) : 0;
	names  = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	ifaces = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	for (i = 0; i < n; i++) {
		SCNetworkInterfaceRef	member;
		CFStringRef		bsd;

		member = (SCNetworkInterfaceRef)
			 CFArrayGetValueAtIndex(members, i);
		bsd = isA_SCNetworkInterface(member)
		      ? SCNetworkInterfaceGetBSDName(member) : NULL;
		if (bsd == NULL) {
			CFRelease(names);
			CFRelease(ifaces);
			_SCErrorSet(kSCStatusInvalidArgument);
			return FALSE;
		}
		CFArrayAppendValue(names, bsd);
		CFArrayAppendValue(ifaces, member);
	}

	if (!__bridgeStoreSet(ip, kSCPropVirtualInterfaces, names)) {
		CFRelease(names);
		CFRelease(ifaces);
		return FALSE;
	}
	CFRelease(names);

	/* update the in-memory member list */
	if (ip->member_interfaces != NULL) {
		CFRelease(ip->member_interfaces);
	}
	ip->member_interfaces = ifaces;
	return TRUE;
}

Boolean
SCBridgeInterfaceSetLocalizedDisplayName(SCBridgeInterfaceRef bridge,
					 CFStringRef newName)
{
	SCNetworkInterfacePrivateRef	ip	= (SCNetworkInterfacePrivateRef)bridge;

	if (!isA_SCBridgeInterface(bridge)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if ((newName == NULL) ||
	    (CFGetTypeID(newName) != CFStringGetTypeID())) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if (!__bridgeStoreSet(ip, kSCPropUserDefinedName, newName)) {
		return FALSE;
	}
	if (ip->localized_name != NULL) {
		CFRelease(ip->localized_name);
	}
	ip->localized_name = CFStringCreateCopy(NULL, newName);
	return TRUE;
}

Boolean
SCBridgeInterfaceSetOptions(SCBridgeInterfaceRef bridge,
			    CFDictionaryRef newOptions)
{
	SCNetworkInterfacePrivateRef	ip	= (SCNetworkInterfacePrivateRef)bridge;

	if (!isA_SCBridgeInterface(bridge)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if ((newOptions != NULL) &&
	    (CFGetTypeID(newOptions) != CFDictionaryGetTypeID())) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if (!__bridgeStoreSet(ip, kSCPropVirtualOptions, newOptions)) {
		return FALSE;
	}
	if (ip->virtual_options != NULL) {
		CFRelease(ip->virtual_options);
		ip->virtual_options = NULL;
	}
	if (newOptions != NULL) {
		ip->virtual_options = CFDictionaryCreateCopy(NULL, newOptions);
	}
	return TRUE;
}

Boolean
SCBridgeInterfaceSetAllowConfiguredMembers(SCBridgeInterfaceRef bridge,
					   Boolean allow)
{
	CFDictionaryRef		options;
	CFMutableDictionaryRef	newOptions;
	Boolean			ok;

	if (!isA_SCBridgeInterface(bridge)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	options = ((SCNetworkInterfacePrivateRef)bridge)->virtual_options;
	if (options != NULL) {
		newOptions = CFDictionaryCreateMutableCopy(NULL, 0, options);
	} else {
		newOptions = CFDictionaryCreateMutable(NULL, 0,
					&kCFTypeDictionaryKeyCallBacks,
					&kCFTypeDictionaryValueCallBacks);
	}
	if (allow) {
		CFDictionarySetValue(newOptions, kAllowConfiguredMembers,
				     kCFBooleanTrue);
	} else {
		CFDictionaryRemoveValue(newOptions, kAllowConfiguredMembers);
	}
	ok = SCBridgeInterfaceSetOptions(bridge, newOptions);
	CFRelease(newOptions);
	return ok;
}

Boolean
SCBridgeInterfaceGetAllowConfiguredMembers(SCBridgeInterfaceRef bridge)
{
	CFDictionaryRef	options;
	CFTypeRef	val;

	if (!isA_SCBridgeInterface(bridge)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	options = ((SCNetworkInterfacePrivateRef)bridge)->virtual_options;
	if (options == NULL) {
		return FALSE;
	}
	val = CFDictionaryGetValue(options, kAllowConfiguredMembers);
	return ((val != NULL) &&
		(CFGetTypeID(val) == CFBooleanGetTypeID()) &&
		CFBooleanGetValue(val));
}
