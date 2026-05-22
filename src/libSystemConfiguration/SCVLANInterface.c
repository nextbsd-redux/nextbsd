/*
 * SCVLANInterface.c — the SCVLANInterface API (freebsd-launchd-mach port
 * of Apple's SystemConfiguration.fproj VLANConfiguration.c).
 *
 * A VLAN interface is a virtual network interface: an 802.1Q tag on top
 * of a physical interface. It is an SCNetworkInterface of type VLAN
 * (SCVLANInterfaceRef is an alias of SCNetworkInterfaceRef). VLANs are
 * persisted in the preferences plist at
 * /VirtualNetworkInterfaces/VLAN/<vlanName>, each entry recording the
 * physical interface's BSD name, the tag, a display name and options.
 *
 * Apple's VLANConfiguration.c also reflects the live kernel state
 * (SIOCGIFVLAN) and cross-references Bond / Bridge membership for the
 * "available physical interfaces" query; the port keeps the stored
 * configuration model.
 */

#include "SCNetworkConfigurationInternal.h"
#include "SCInternal.h"

#include <stdlib.h>


#pragma mark -
#pragma mark Helpers

/* TRUE iff `vlan` is a live SCNetworkInterface of type VLAN */
static Boolean
isA_SCVLANInterface(SCVLANInterfaceRef vlan)
{
	return (isA_SCNetworkInterface(vlan) &&
		CFEqual(((SCNetworkInterfacePrivateRef)vlan)->interface_type,
			kSCNetworkInterfaceTypeVLAN));
}

/* "/VirtualNetworkInterfaces/VLAN" */
static CFStringRef
__vlanContainerPath(void)
{
	return CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@/%@"),
					kSCPrefVirtualNetworkInterfaces,
					kSCNetworkInterfaceTypeVLAN);
}

/* "/VirtualNetworkInterfaces/VLAN/<vlanName>" */
static CFStringRef
__vlanPath(CFStringRef vlanName)
{
	return CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@/%@/%@"),
					kSCPrefVirtualNetworkInterfaces,
					kSCNetworkInterfaceTypeVLAN, vlanName);
}

/* allocate a VLAN interface object named `vlan_if` (e.g. "vlan0") */
static SCVLANInterfaceRef
_SCVLANInterfaceCreatePrivate(CFAllocatorRef allocator, CFStringRef vlan_if)
{
	SCNetworkInterfacePrivateRef	ip;

	ip = __SCNetworkInterfaceCreatePrivate(allocator);
	if (ip == NULL) {
		return NULL;
	}
	ip->interface_type =
		(CFStringRef)CFRetain(kSCNetworkInterfaceTypeVLAN);
	ip->entity_device  = CFStringCreateCopy(allocator, vlan_if);
	ip->builtin        = TRUE;
	ip->localized_name = CFStringCreateWithCString(NULL, "VLAN",
						       kCFStringEncodingUTF8);
	ip->name           = (CFStringRef)CFRetain(ip->localized_name);
	/* a VLAN can carry the usual protocols, so a service can use it */
	ip->supported_protocol_types = __SCNetworkInterfaceCopyProtocolTypes();
	return (SCVLANInterfaceRef)ip;
}

/* TRUE iff `tag` is a CFNumber in the valid 802.1Q range (1..4094) */
static Boolean
__vlanTagIsValid(CFNumberRef tag)
{
	int	value;

	if ((tag == NULL) || (CFGetTypeID(tag) != CFNumberGetTypeID())) {
		return FALSE;
	}
	if (!CFNumberGetValue(tag, kCFNumberIntType, &value)) {
		return FALSE;
	}
	return ((value >= 1) && (value <= 4094));
}

/* the stored VLAN with this physical interface + tag, or NULL */
static SCVLANInterfaceRef
__findVLANInterfaceAndTag(SCPreferencesRef prefs,
			  SCNetworkInterfaceRef physical, CFNumberRef tag)
{
	CFArrayRef		vlans;
	CFIndex			i, n;
	SCVLANInterfaceRef	found	= NULL;

	vlans = SCVLANInterfaceCopyAll(prefs);
	n     = (vlans != NULL) ? CFArrayGetCount(vlans) : 0;
	for (i = 0; i < n; i++) {
		SCVLANInterfaceRef	vlan;
		SCNetworkInterfaceRef	vp;
		CFNumberRef		vt;

		vlan = (SCVLANInterfaceRef)CFArrayGetValueAtIndex(vlans, i);
		vp   = SCVLANInterfaceGetPhysicalInterface(vlan);
		vt   = SCVLANInterfaceGetTag(vlan);
		if ((vp != NULL) && (vt != NULL) &&
		    CFEqual(physical, vp) && CFEqual(tag, vt)) {
			found = (SCVLANInterfaceRef)CFRetain(vlan);
			break;
		}
	}
	if (vlans != NULL) {
		CFRelease(vlans);
	}
	return found;
}


#pragma mark -
#pragma mark SCVLANInterface APIs

CFArrayRef
SCVLANInterfaceCopyAll(SCPreferencesRef prefs)
{
	CFMutableArrayRef	array;
	CFDictionaryRef		container;
	const void		**keys;
	const void		**vals;
	CFIndex			i, n;
	CFStringRef		path;

	array = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

	path      = __vlanContainerPath();
	container = SCPreferencesPathGetValue(prefs, path);
	CFRelease(path);
	if ((container == NULL) ||
	    (CFGetTypeID(container) != CFDictionaryGetTypeID())) {
		return array;
	}

	n    = CFDictionaryGetCount(container);
	if (n <= 0) {
		return array;
	}
	keys = (const void **)malloc((size_t)n * sizeof(void *));
	vals = (const void **)malloc((size_t)n * sizeof(void *));
	CFDictionaryGetKeysAndValues(container, keys, vals);

	for (i = 0; i < n; i++) {
		SCNetworkInterfacePrivateRef	ip;
		SCVLANInterfaceRef		vlan;
		CFDictionaryRef			info	= (CFDictionaryRef)vals[i];
		CFStringRef			physIf;
		CFNumberRef			tag;
		CFStringRef			name;
		CFDictionaryRef			options;

		if (CFGetTypeID(info) != CFDictionaryGetTypeID()) {
			continue;
		}
		physIf = CFDictionaryGetValue(info, kSCPropVLANInterface);
		tag    = CFDictionaryGetValue(info, kSCPropVLANTag);
		if ((physIf == NULL) ||
		    (CFGetTypeID(physIf) != CFStringGetTypeID()) ||
		    (tag == NULL) ||
		    (CFGetTypeID(tag) != CFNumberGetTypeID())) {
			/* an incomplete entry — skip it */
			continue;
		}

		vlan = _SCVLANInterfaceCreatePrivate(NULL, (CFStringRef)keys[i]);
		if (vlan == NULL) {
			continue;
		}
		ip = (SCNetworkInterfacePrivateRef)vlan;
		ip->prefs         = (SCPreferencesRef)CFRetain(prefs);
		ip->vlan_physical = __SCNetworkInterfaceCreateWithBSDName(NULL,
									 physIf);
		ip->vlan_tag      = (CFNumberRef)CFRetain(tag);

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

		CFArrayAppendValue(array, vlan);
		CFRelease(vlan);
	}

	free(keys);
	free(vals);
	return array;
}

CFArrayRef
SCVLANInterfaceCopyAvailablePhysicalInterfaces(void)
{
	/*
	 * Every hardware interface can host a VLAN. Apple additionally
	 * folds in Bond / Bridge interfaces and excludes interfaces that
	 * are already members of one — handled once those land.
	 */
	return SCNetworkInterfaceCopyAll();
}

SCVLANInterfaceRef
SCVLANInterfaceCreate(SCPreferencesRef prefs, SCNetworkInterfaceRef physical,
		      CFNumberRef tag)
{
	SCVLANInterfaceRef	existing;
	SCVLANInterfaceRef	vlan	= NULL;
	int			i;

	if (prefs == NULL) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	if (!isA_SCNetworkInterface(physical) || !__vlanTagIsValid(tag)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	/* the physical interface + tag pair must be unused */
	existing = __findVLANInterfaceAndTag(prefs, physical, tag);
	if (existing != NULL) {
		CFRelease(existing);
		_SCErrorSet(kSCStatusKeyExists);
		return NULL;
	}

	/* claim the first free vlanN name */
	for (i = 0; i < 1024; i++) {
		CFDictionaryRef	empty;
		CFStringRef	path;
		CFStringRef	vlan_if;
		Boolean		taken;

		vlan_if = CFStringCreateWithFormat(NULL, NULL,
						   CFSTR("vlan%d"), i);
		path    = __vlanPath(vlan_if);
		taken   = (SCPreferencesPathGetValue(prefs, path) != NULL);
		if (taken) {
			CFRelease(path);
			CFRelease(vlan_if);
			continue;
		}

		empty = CFDictionaryCreate(NULL, NULL, NULL, 0,
					   &kCFTypeDictionaryKeyCallBacks,
					   &kCFTypeDictionaryValueCallBacks);
		if (!SCPreferencesPathSetValue(prefs, path, empty)) {
			CFRelease(empty);
			CFRelease(path);
			CFRelease(vlan_if);
			_SCErrorSet(kSCStatusFailed);
			return NULL;
		}
		CFRelease(empty);
		CFRelease(path);

		vlan = _SCVLANInterfaceCreatePrivate(NULL, vlan_if);
		CFRelease(vlan_if);
		if (vlan != NULL) {
			((SCNetworkInterfacePrivateRef)vlan)->prefs =
				(SCPreferencesRef)CFRetain(prefs);
		}
		break;
	}
	if (vlan == NULL) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}

	/* record the physical interface + tag */
	if (!SCVLANInterfaceSetPhysicalInterfaceAndTag(vlan, physical, tag)) {
		CFRelease(vlan);
		return NULL;
	}
	return vlan;
}

Boolean
SCVLANInterfaceRemove(SCVLANInterfaceRef vlan)
{
	SCNetworkInterfacePrivateRef	ip	= (SCNetworkInterfacePrivateRef)vlan;
	CFStringRef			path;
	Boolean				ok;

	if (!isA_SCVLANInterface(vlan) || (ip->prefs == NULL)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	path = __vlanPath(ip->entity_device);
	ok   = SCPreferencesPathRemoveValue(ip->prefs, path);
	CFRelease(path);
	return ok;
}

SCNetworkInterfaceRef
SCVLANInterfaceGetPhysicalInterface(SCVLANInterfaceRef vlan)
{
	if (!isA_SCVLANInterface(vlan)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	return ((SCNetworkInterfacePrivateRef)vlan)->vlan_physical;
}

CFNumberRef
SCVLANInterfaceGetTag(SCVLANInterfaceRef vlan)
{
	if (!isA_SCVLANInterface(vlan)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	return ((SCNetworkInterfacePrivateRef)vlan)->vlan_tag;
}

CFDictionaryRef
SCVLANInterfaceGetOptions(SCVLANInterfaceRef vlan)
{
	if (!isA_SCVLANInterface(vlan)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	return ((SCNetworkInterfacePrivateRef)vlan)->virtual_options;
}

/*
 * Rewrite the stored entry for `vlan` with the value at `key` set to
 * `value` (NULL removes). Returns FALSE (and sets SCError) if the entry
 * is missing or the write fails. A vlan with no prefs is in-memory only,
 * which the callers treat as success.
 */
static Boolean
__vlanStoreSet(SCNetworkInterfacePrivateRef ip, CFStringRef key,
	       CFTypeRef value)
{
	CFDictionaryRef		dict;
	CFMutableDictionaryRef	newDict;
	CFStringRef		path;
	Boolean			ok;

	if (ip->prefs == NULL) {
		return TRUE;
	}
	path = __vlanPath(ip->entity_device);
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

Boolean
SCVLANInterfaceSetPhysicalInterfaceAndTag(SCVLANInterfaceRef vlan,
					  SCNetworkInterfaceRef physical,
					  CFNumberRef tag)
{
	SCNetworkInterfacePrivateRef	ip	= (SCNetworkInterfacePrivateRef)vlan;
	CFStringRef			bsdName;

	if (!isA_SCVLANInterface(vlan)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if (!isA_SCNetworkInterface(physical) || !__vlanTagIsValid(tag)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	bsdName = SCNetworkInterfaceGetBSDName(physical);
	if (bsdName == NULL) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	if (ip->prefs != NULL) {
		SCVLANInterfaceRef	other;

		/* the physical + tag pair must not be used by another VLAN */
		other = __findVLANInterfaceAndTag(ip->prefs, physical, tag);
		if (other != NULL) {
			Boolean	isSelf = CFEqual(vlan, other);

			CFRelease(other);
			if (!isSelf) {
				_SCErrorSet(kSCStatusKeyExists);
				return FALSE;
			}
		}
		if (!__vlanStoreSet(ip, kSCPropVLANInterface, bsdName)) {
			return FALSE;
		}
		if (!__vlanStoreSet(ip, kSCPropVLANTag, tag)) {
			return FALSE;
		}
	}

	/* update the in-memory object */
	if (ip->vlan_physical != NULL) {
		CFRelease(ip->vlan_physical);
	}
	ip->vlan_physical = (SCNetworkInterfaceRef)CFRetain(physical);
	if (ip->vlan_tag != NULL) {
		CFRelease(ip->vlan_tag);
	}
	ip->vlan_tag = (CFNumberRef)CFRetain(tag);
	return TRUE;
}

Boolean
SCVLANInterfaceSetLocalizedDisplayName(SCVLANInterfaceRef vlan,
				       CFStringRef newName)
{
	SCNetworkInterfacePrivateRef	ip	= (SCNetworkInterfacePrivateRef)vlan;

	if (!isA_SCVLANInterface(vlan)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if ((newName == NULL) ||
	    (CFGetTypeID(newName) != CFStringGetTypeID())) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if (!__vlanStoreSet(ip, kSCPropUserDefinedName, newName)) {
		return FALSE;
	}
	if (ip->localized_name != NULL) {
		CFRelease(ip->localized_name);
	}
	ip->localized_name = CFStringCreateCopy(NULL, newName);
	return TRUE;
}

Boolean
SCVLANInterfaceSetOptions(SCVLANInterfaceRef vlan, CFDictionaryRef newOptions)
{
	SCNetworkInterfacePrivateRef	ip	= (SCNetworkInterfacePrivateRef)vlan;

	if (!isA_SCVLANInterface(vlan)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if ((newOptions != NULL) &&
	    (CFGetTypeID(newOptions) != CFDictionaryGetTypeID())) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if (!__vlanStoreSet(ip, kSCPropVirtualOptions, newOptions)) {
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
