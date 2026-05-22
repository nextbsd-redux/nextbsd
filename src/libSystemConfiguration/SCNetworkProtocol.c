/*
 * SCNetworkProtocol.c — the SCNetworkProtocol object (freebsd-launchd-
 * mach port of Apple's SystemConfiguration.fproj SCNetworkProtocol.c).
 *
 * An SCNetworkProtocol is one protocol (IPv4, IPv6, DNS, Proxies, SMB)
 * configured on a network service. It is a thin handle: the protocol's
 * configuration lives in the preferences plist at
 * /NetworkServices/<serviceID>/<protocolType>, and a reserved
 * "__INACTIVE__" key in that dictionary marks the protocol disabled.
 */

#include "SCNetworkConfigurationInternal.h"
#include "SCInternal.h"

#include <pthread.h>


#pragma mark -
#pragma mark CoreFoundation runtime type

static CFTypeID	__kSCNetworkProtocolTypeID	= _kCFRuntimeNotATypeID;

static void
__SCNetworkProtocolDeallocate(CFTypeRef cf)
{
	SCNetworkProtocolPrivateRef	pp	= (SCNetworkProtocolPrivateRef)cf;

	if (pp->entityID != NULL)	CFRelease(pp->entityID);
	if (pp->service != NULL)	CFRelease(pp->service);
}

static Boolean
__SCNetworkProtocolEqual(CFTypeRef cf1, CFTypeRef cf2)
{
	SCNetworkProtocolPrivateRef	p1	= (SCNetworkProtocolPrivateRef)cf1;
	SCNetworkProtocolPrivateRef	p2	= (SCNetworkProtocolPrivateRef)cf2;

	if (p1 == p2) {
		return TRUE;
	}
	if (!CFEqual(p1->entityID, p2->entityID)) {
		return FALSE;	/* a different protocol type */
	}
	if (p1->service == p2->service) {
		return TRUE;
	}
	return ((p1->service != NULL) && (p2->service != NULL) &&
		CFEqual(p1->service, p2->service));
}

static CFHashCode
__SCNetworkProtocolHash(CFTypeRef cf)
{
	return CFHash(((SCNetworkProtocolPrivateRef)cf)->entityID);
}

static CFStringRef
__SCNetworkProtocolCopyDescription(CFTypeRef cf)
{
	SCNetworkProtocolPrivateRef	pp	= (SCNetworkProtocolPrivateRef)cf;

	return CFStringCreateWithFormat(NULL, NULL,
			CFSTR("<SCNetworkProtocol %@>"), pp->entityID);
}

static const CFRuntimeClass __SCNetworkProtocolClass = {
	.version	= 0,
	.className	= "SCNetworkProtocol",
	.finalize	= __SCNetworkProtocolDeallocate,
	.equal		= __SCNetworkProtocolEqual,
	.hash		= __SCNetworkProtocolHash,
	.copyDebugDesc	= __SCNetworkProtocolCopyDescription,
};

static void
__SCNetworkProtocolClassInitialize(void)
{
	__kSCNetworkProtocolTypeID =
		_CFRuntimeRegisterClass(&__SCNetworkProtocolClass);
}

CFTypeID
SCNetworkProtocolGetTypeID(void)
{
	static pthread_once_t	once	= PTHREAD_ONCE_INIT;

	(void) pthread_once(&once, __SCNetworkProtocolClassInitialize);
	return __kSCNetworkProtocolTypeID;
}

SCNetworkProtocolPrivateRef
__SCNetworkProtocolCreatePrivate(CFAllocatorRef		allocator,
				 CFStringRef		entityID,
				 SCNetworkServiceRef	service)
{
	SCNetworkProtocolPrivateRef	pp;
	CFIndex				extra;

	extra = sizeof(SCNetworkProtocolPrivate) - sizeof(CFRuntimeBase);
	pp = (SCNetworkProtocolPrivateRef)
	     _CFRuntimeCreateInstance(allocator,
				      SCNetworkProtocolGetTypeID(),
				      extra, NULL);
	if (pp == NULL) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}
	pp->entityID = CFStringCreateCopy(NULL, entityID);
	pp->service  = (SCNetworkServiceRef)CFRetain(service);
	return pp;
}

Boolean
__SCNetworkProtocolIsValidType(CFStringRef protocolType)
{
	static const CFStringRef * const	valid[] = {
		&kSCNetworkProtocolTypeDNS,
		&kSCNetworkProtocolTypeIPv4,
		&kSCNetworkProtocolTypeIPv6,
		&kSCNetworkProtocolTypeProxies,
		&kSCNetworkProtocolTypeSMB,
	};
	size_t	i;

	if ((protocolType == NULL) ||
	    (CFGetTypeID(protocolType) != CFStringGetTypeID())) {
		return FALSE;
	}
	for (i = 0; i < (sizeof(valid) / sizeof(valid[0])); i++) {
		if (CFEqual(protocolType, *valid[i])) {
			return TRUE;
		}
	}
	/* a user-defined protocol type (e.g. com.example.myProtocol) */
	return (CFStringFind(protocolType, CFSTR("."), 0).location
		!= kCFNotFound);
}


#pragma mark -
#pragma mark Accessors

/* the preferences path of this protocol's configuration entity */
static CFStringRef
__protocolPath(SCNetworkProtocolPrivateRef pp)
{
	SCNetworkServicePrivateRef	sp =
		(SCNetworkServicePrivateRef)pp->service;

	return __SCNetworkServiceEntityPath(sp->serviceID, pp->entityID);
}

CFStringRef
SCNetworkProtocolGetProtocolType(SCNetworkProtocolRef protocol)
{
	if (!isA_SCNetworkProtocol(protocol)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	return ((SCNetworkProtocolPrivateRef)protocol)->entityID;
}

CFDictionaryRef
SCNetworkProtocolGetConfiguration(SCNetworkProtocolRef protocol)
{
	SCNetworkProtocolPrivateRef	pp	= (SCNetworkProtocolPrivateRef)protocol;
	SCNetworkServicePrivateRef	sp;
	CFDictionaryRef			config;
	CFStringRef			path;

	if (!isA_SCNetworkProtocol(protocol)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	sp     = (SCNetworkServicePrivateRef)pp->service;
	path   = __protocolPath(pp);
	config = SCPreferencesPathGetValue(sp->prefs, path);
	CFRelease(path);

	if ((config == NULL) ||
	    (CFGetTypeID(config) != CFDictionaryGetTypeID())) {
		return NULL;
	}
	/* an empty entity — or one holding only the disabled marker — has
	 * no configuration to report */
	if (CFDictionaryGetCount(config) == 0) {
		return NULL;
	}
	if ((CFDictionaryGetCount(config) == 1) &&
	    CFDictionaryContainsKey(config, kSCResvInactive)) {
		return NULL;
	}
	return config;
}

Boolean
SCNetworkProtocolSetConfiguration(SCNetworkProtocolRef protocol,
				  CFDictionaryRef config)
{
	SCNetworkProtocolPrivateRef	pp	= (SCNetworkProtocolPrivateRef)protocol;
	SCNetworkServicePrivateRef	sp;
	CFDictionaryRef			cur;
	CFMutableDictionaryRef		newConfig;
	CFStringRef			path;
	Boolean				ok;

	if (!isA_SCNetworkProtocol(protocol)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if ((config != NULL) &&
	    (CFGetTypeID(config) != CFDictionaryGetTypeID())) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	sp   = (SCNetworkServicePrivateRef)pp->service;
	path = __protocolPath(pp);

	/* the new configuration preserves the current enabled state */
	cur = SCPreferencesPathGetValue(sp->prefs, path);
	if ((cur != NULL) && (CFGetTypeID(cur) != CFDictionaryGetTypeID())) {
		cur = NULL;
	}
	if (config != NULL) {
		newConfig = CFDictionaryCreateMutableCopy(NULL, 0, config);
	} else {
		newConfig = CFDictionaryCreateMutable(NULL, 0,
					&kCFTypeDictionaryKeyCallBacks,
					&kCFTypeDictionaryValueCallBacks);
	}
	if ((cur != NULL) &&
	    CFDictionaryContainsKey(cur, kSCResvInactive)) {
		CFDictionarySetValue(newConfig, kSCResvInactive,
				     kCFBooleanTrue);
	} else {
		CFDictionaryRemoveValue(newConfig, kSCResvInactive);
	}

	ok = SCPreferencesPathSetValue(sp->prefs, path, newConfig);
	CFRelease(newConfig);
	CFRelease(path);
	return ok;
}

Boolean
SCNetworkProtocolGetEnabled(SCNetworkProtocolRef protocol)
{
	SCNetworkProtocolPrivateRef	pp	= (SCNetworkProtocolPrivateRef)protocol;
	SCNetworkServicePrivateRef	sp;
	CFDictionaryRef			config;
	CFStringRef			path;
	Boolean				enabled;

	if (!isA_SCNetworkProtocol(protocol)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	sp      = (SCNetworkServicePrivateRef)pp->service;
	path    = __protocolPath(pp);
	config  = SCPreferencesPathGetValue(sp->prefs, path);
	enabled = !((config != NULL) &&
		    (CFGetTypeID(config) == CFDictionaryGetTypeID()) &&
		    CFDictionaryContainsKey(config, kSCResvInactive));
	CFRelease(path);
	return enabled;
}

Boolean
SCNetworkProtocolSetEnabled(SCNetworkProtocolRef protocol, Boolean enabled)
{
	SCNetworkProtocolPrivateRef	pp	= (SCNetworkProtocolPrivateRef)protocol;
	SCNetworkServicePrivateRef	sp;
	CFDictionaryRef			cur;
	CFMutableDictionaryRef		newConfig;
	CFStringRef			path;
	Boolean				ok;

	if (!isA_SCNetworkProtocol(protocol)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	sp   = (SCNetworkServicePrivateRef)pp->service;
	path = __protocolPath(pp);

	cur = SCPreferencesPathGetValue(sp->prefs, path);
	if ((cur != NULL) && (CFGetTypeID(cur) != CFDictionaryGetTypeID())) {
		cur = NULL;
	}
	if (cur != NULL) {
		newConfig = CFDictionaryCreateMutableCopy(NULL, 0, cur);
	} else {
		newConfig = CFDictionaryCreateMutable(NULL, 0,
					&kCFTypeDictionaryKeyCallBacks,
					&kCFTypeDictionaryValueCallBacks);
	}
	if (enabled) {
		CFDictionaryRemoveValue(newConfig, kSCResvInactive);
	} else {
		CFDictionarySetValue(newConfig, kSCResvInactive,
				     kCFBooleanTrue);
	}

	ok = SCPreferencesPathSetValue(sp->prefs, path, newConfig);
	CFRelease(newConfig);
	CFRelease(path);
	return ok;
}
