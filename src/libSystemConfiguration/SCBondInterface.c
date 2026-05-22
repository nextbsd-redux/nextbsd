/*
 * SCBondInterface.c — the SCBondInterface API (freebsd-launchd-mach port
 * of Apple's SystemConfiguration.fproj BondConfiguration.c).
 *
 * A bond interface aggregates several physical interfaces into one
 * (FreeBSD lagg(4)). It is an SCNetworkInterface of type Bond
 * (SCBondInterfaceRef aliases SCNetworkInterfaceRef). Bonds are
 * persisted in the preferences plist at
 * /VirtualNetworkInterfaces/Bond/<bondName>, each entry recording the
 * member interfaces' BSD names, a display name and options.
 *
 * Apple's BondConfiguration.c also reports live aggregation status
 * (SCBondInterfaceCopyStatus, via lagg ioctls); the port keeps the
 * stored configuration model.
 */

#include "SCNetworkConfigurationInternal.h"
#include "SCInternal.h"

#include <stdlib.h>


#pragma mark -
#pragma mark Helpers

/* TRUE iff `bond` is a live SCNetworkInterface of type Bond */
static Boolean
isA_SCBondInterface(SCBondInterfaceRef bond)
{
	return (isA_SCNetworkInterface(bond) &&
		CFEqual(((SCNetworkInterfacePrivateRef)bond)->interface_type,
			kSCNetworkInterfaceTypeBond));
}

/* "/VirtualNetworkInterfaces/Bond" */
static CFStringRef
__bondContainerPath(void)
{
	return CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@/%@"),
					kSCPrefVirtualNetworkInterfaces,
					kSCNetworkInterfaceTypeBond);
}

/* "/VirtualNetworkInterfaces/Bond/<bondName>" */
static CFStringRef
__bondPath(CFStringRef bondName)
{
	return CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@/%@/%@"),
					kSCPrefVirtualNetworkInterfaces,
					kSCNetworkInterfaceTypeBond, bondName);
}

/* allocate a Bond interface object named `bond_if` (e.g. "bond0") */
static SCBondInterfaceRef
_SCBondInterfaceCreatePrivate(CFAllocatorRef allocator, CFStringRef bond_if)
{
	SCNetworkInterfacePrivateRef	ip;

	ip = __SCNetworkInterfaceCreatePrivate(allocator);
	if (ip == NULL) {
		return NULL;
	}
	ip->interface_type =
		(CFStringRef)CFRetain(kSCNetworkInterfaceTypeBond);
	ip->entity_device  = CFStringCreateCopy(allocator, bond_if);
	ip->builtin        = TRUE;
	ip->localized_name = CFStringCreateWithCString(NULL, "Bond",
						       kCFStringEncodingUTF8);
	ip->name           = (CFStringRef)CFRetain(ip->localized_name);
	/* a bond carries the usual protocols, so a service can use it */
	ip->supported_protocol_types = __SCNetworkInterfaceCopyProtocolTypes();
	/* a fresh bond has an (empty) member list, never NULL */
	ip->member_interfaces =
		CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);
	return (SCBondInterfaceRef)ip;
}

/*
 * Rewrite the stored entry for `bond` with the value at `key` set to
 * `value` (NULL removes). A bond with no prefs is in-memory only, which
 * the callers treat as success.
 */
static Boolean
__bondStoreSet(SCNetworkInterfacePrivateRef ip, CFStringRef key,
	       CFTypeRef value)
{
	CFDictionaryRef		dict;
	CFMutableDictionaryRef	newDict;
	CFStringRef		path;
	Boolean			ok;

	if (ip->prefs == NULL) {
		return TRUE;
	}
	path = __bondPath(ip->entity_device);
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
#pragma mark SCBondInterface APIs

CFArrayRef
SCBondInterfaceCopyAll(SCPreferencesRef prefs)
{
	CFMutableArrayRef	array;
	CFDictionaryRef		container;
	const void		**keys;
	const void		**vals;
	CFIndex			i, n;
	CFStringRef		path;

	array = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

	path      = __bondContainerPath();
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
		SCBondInterfaceRef		bond;
		CFDictionaryRef			info	= (CFDictionaryRef)vals[i];
		CFArrayRef			members;
		CFDictionaryRef			options;
		CFStringRef			name;
		CFMutableArrayRef		ifaces;
		CFIndex				j, m;

		if (CFGetTypeID(info) != CFDictionaryGetTypeID()) {
			continue;
		}
		bond = _SCBondInterfaceCreatePrivate(NULL, (CFStringRef)keys[i]);
		if (bond == NULL) {
			continue;
		}
		ip        = (SCNetworkInterfacePrivateRef)bond;
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

		CFArrayAppendValue(array, bond);
		CFRelease(bond);
	}

	free(keys);
	free(vals);
	return array;
}

CFArrayRef
SCBondInterfaceCopyAvailableMemberInterfaces(SCPreferencesRef prefs)
{
	CFMutableArrayRef	available;
	CFArrayRef		all;
	CFArrayRef		bonds;
	CFMutableSetRef		excluded;
	CFIndex			i, n;

	available = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	excluded  = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);

	/* an interface already in a bond is not available */
	bonds = SCBondInterfaceCopyAll(prefs);
	n     = (bonds != NULL) ? CFArrayGetCount(bonds) : 0;
	for (i = 0; i < n; i++) {
		CFArrayRef	members;
		CFIndex		j, m;

		members = SCBondInterfaceGetMemberInterfaces(
				(SCBondInterfaceRef)
				CFArrayGetValueAtIndex(bonds, i));
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
	if (bonds != NULL) {
		CFRelease(bonds);
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

SCBondInterfaceRef
SCBondInterfaceCreate(SCPreferencesRef prefs)
{
	SCBondInterfaceRef	bond	= NULL;
	int			i;

	if (prefs == NULL) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	/* claim the first free bondN name */
	for (i = 0; i < 1024; i++) {
		CFMutableDictionaryRef	entry;
		CFArrayRef		empty;
		CFStringRef		bond_if;
		CFStringRef		path;
		Boolean			ok;

		bond_if = CFStringCreateWithFormat(NULL, NULL,
						   CFSTR("bond%d"), i);
		path    = __bondPath(bond_if);
		if (SCPreferencesPathGetValue(prefs, path) != NULL) {
			CFRelease(path);
			CFRelease(bond_if);
			continue;
		}

		/* a new bond starts with an empty member list */
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
			CFRelease(bond_if);
			_SCErrorSet(kSCStatusFailed);
			return NULL;
		}

		bond = _SCBondInterfaceCreatePrivate(NULL, bond_if);
		CFRelease(bond_if);
		if (bond != NULL) {
			((SCNetworkInterfacePrivateRef)bond)->prefs =
				(SCPreferencesRef)CFRetain(prefs);
		}
		break;
	}
	if (bond == NULL) {
		_SCErrorSet(kSCStatusFailed);
	}
	return bond;
}

Boolean
SCBondInterfaceRemove(SCBondInterfaceRef bond)
{
	SCNetworkInterfacePrivateRef	ip	= (SCNetworkInterfacePrivateRef)bond;
	CFStringRef			path;
	Boolean				ok;

	if (!isA_SCBondInterface(bond) || (ip->prefs == NULL)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	path = __bondPath(ip->entity_device);
	ok   = SCPreferencesPathRemoveValue(ip->prefs, path);
	CFRelease(path);
	return ok;
}

CFArrayRef
SCBondInterfaceGetMemberInterfaces(SCBondInterfaceRef bond)
{
	if (!isA_SCBondInterface(bond)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	return ((SCNetworkInterfacePrivateRef)bond)->member_interfaces;
}

CFDictionaryRef
SCBondInterfaceGetOptions(SCBondInterfaceRef bond)
{
	if (!isA_SCBondInterface(bond)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	return ((SCNetworkInterfacePrivateRef)bond)->virtual_options;
}

Boolean
SCBondInterfaceSetMemberInterfaces(SCBondInterfaceRef bond, CFArrayRef members)
{
	SCNetworkInterfacePrivateRef	ip	= (SCNetworkInterfacePrivateRef)bond;
	CFMutableArrayRef		names;
	CFMutableArrayRef		ifaces;
	CFIndex				i, n;

	if (!isA_SCBondInterface(bond)) {
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

	if (!__bondStoreSet(ip, kSCPropVirtualInterfaces, names)) {
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
SCBondInterfaceSetLocalizedDisplayName(SCBondInterfaceRef bond,
				       CFStringRef newName)
{
	SCNetworkInterfacePrivateRef	ip	= (SCNetworkInterfacePrivateRef)bond;

	if (!isA_SCBondInterface(bond)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if ((newName == NULL) ||
	    (CFGetTypeID(newName) != CFStringGetTypeID())) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if (!__bondStoreSet(ip, kSCPropUserDefinedName, newName)) {
		return FALSE;
	}
	if (ip->localized_name != NULL) {
		CFRelease(ip->localized_name);
	}
	ip->localized_name = CFStringCreateCopy(NULL, newName);
	return TRUE;
}

Boolean
SCBondInterfaceSetOptions(SCBondInterfaceRef bond, CFDictionaryRef newOptions)
{
	SCNetworkInterfacePrivateRef	ip	= (SCNetworkInterfacePrivateRef)bond;

	if (!isA_SCBondInterface(bond)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if ((newOptions != NULL) &&
	    (CFGetTypeID(newOptions) != CFDictionaryGetTypeID())) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if (!__bondStoreSet(ip, kSCPropVirtualOptions, newOptions)) {
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
