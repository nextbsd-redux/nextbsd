/*
 * SCNetworkSet.c — the SCNetworkSet object (freebsd-launchd-mach port of
 * Apple's SystemConfiguration.fproj SCNetworkSet.c).
 *
 * A network set — a "location" — is an ordered collection of network
 * services. It is persisted in the preferences plist at /Sets/<setID>;
 * a service is a member of a set via a __LINK__ entry at
 * /Sets/<setID>/Network/Service/<serviceID> pointing back at the real
 * /NetworkServices/<serviceID>. The active set is named by the
 * top-level "CurrentSet" preferences key.
 *
 * Apple's SCNetworkSet.c additionally manages the special "Automatic"
 * default set, per-interface "deep" configuration, and VPN selection;
 * the port keeps the core model: create / copy / list a set, name it,
 * add and remove services, the service order, and the current set.
 */

#include "SCNetworkConfigurationInternal.h"
#include "SCInternal.h"

#include <pthread.h>
#include <stdlib.h>


#pragma mark -
#pragma mark CoreFoundation runtime type

static CFTypeID	__kSCNetworkSetTypeID	= _kCFRuntimeNotATypeID;

static void
__SCNetworkSetDeallocate(CFTypeRef cf)
{
	SCNetworkSetPrivateRef	sp	= (SCNetworkSetPrivateRef)cf;

	if (sp->setID != NULL)	CFRelease(sp->setID);
	if (sp->prefs != NULL)	CFRelease(sp->prefs);
	if (sp->name != NULL)	CFRelease(sp->name);
}

static Boolean
__SCNetworkSetEqual(CFTypeRef cf1, CFTypeRef cf2)
{
	SCNetworkSetPrivateRef	s1	= (SCNetworkSetPrivateRef)cf1;
	SCNetworkSetPrivateRef	s2	= (SCNetworkSetPrivateRef)cf2;

	if (s1 == s2) {
		return TRUE;
	}
	return CFEqual(s1->setID, s2->setID);
}

static CFHashCode
__SCNetworkSetHash(CFTypeRef cf)
{
	return CFHash(((SCNetworkSetPrivateRef)cf)->setID);
}

static CFStringRef
__SCNetworkSetCopyDescription(CFTypeRef cf)
{
	SCNetworkSetPrivateRef	sp	= (SCNetworkSetPrivateRef)cf;

	return CFStringCreateWithFormat(NULL, NULL,
			CFSTR("<SCNetworkSet %@>"), sp->setID);
}

static const CFRuntimeClass __SCNetworkSetClass = {
	.version	= 0,
	.className	= "SCNetworkSet",
	.finalize	= __SCNetworkSetDeallocate,
	.equal		= __SCNetworkSetEqual,
	.hash		= __SCNetworkSetHash,
	.copyDebugDesc	= __SCNetworkSetCopyDescription,
};

static void
__SCNetworkSetClassInitialize(void)
{
	__kSCNetworkSetTypeID = _CFRuntimeRegisterClass(&__SCNetworkSetClass);
}

CFTypeID
SCNetworkSetGetTypeID(void)
{
	static pthread_once_t	once	= PTHREAD_ONCE_INIT;

	(void) pthread_once(&once, __SCNetworkSetClassInitialize);
	return __kSCNetworkSetTypeID;
}

SCNetworkSetPrivateRef
__SCNetworkSetCreatePrivate(CFAllocatorRef	allocator,
			    SCPreferencesRef	prefs,
			    CFStringRef		setID)
{
	SCNetworkSetPrivateRef	sp;
	CFIndex			extra;

	extra = sizeof(SCNetworkSetPrivate) - sizeof(CFRuntimeBase);
	sp = (SCNetworkSetPrivateRef)
	     _CFRuntimeCreateInstance(allocator,
				      SCNetworkSetGetTypeID(),
				      extra, NULL);
	if (sp == NULL) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}
	sp->setID = CFStringCreateCopy(NULL, setID);
	sp->prefs = (prefs != NULL)
		    ? (SCPreferencesRef)CFRetain(prefs) : NULL;
	sp->name  = NULL;
	return sp;
}


#pragma mark -
#pragma mark Path helpers

/* "/Sets/<setID>" */
static CFStringRef
__setPath(CFStringRef setID)
{
	return CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@/%@"),
					kSCPrefSets, setID);
}

/*
 * "/Sets/<setID>/Network/Service" (serviceID NULL) or
 * "/Sets/<setID>/Network/Service/<serviceID>"
 */
static CFStringRef
__setServicePath(CFStringRef setID, CFStringRef serviceID)
{
	if (serviceID == NULL) {
		return CFStringCreateWithFormat(NULL, NULL,
				CFSTR("/%@/%@/%@/%@"),
				kSCPrefSets, setID,
				kSCCompNetwork, kSCCompService);
	}
	return CFStringCreateWithFormat(NULL, NULL,
			CFSTR("/%@/%@/%@/%@/%@"),
			kSCPrefSets, setID,
			kSCCompNetwork, kSCCompService, serviceID);
}

/* "/Sets/<setID>/Network/Global/IPv4" — holds the service order */
static CFStringRef
__setGlobalIPv4Path(CFStringRef setID)
{
	return CFStringCreateWithFormat(NULL, NULL,
			CFSTR("/%@/%@/%@/%@/%@"),
			kSCPrefSets, setID,
			kSCCompNetwork, kSCCompGlobal, kSCEntNetIPv4);
}

/* the __LINK__ target for a service membership: "/NetworkServices/<id>" */
static CFStringRef
__netServicePath(CFStringRef serviceID)
{
	return CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@/%@"),
					kSCPrefNetworkServices, serviceID);
}

/*
 * Copy the keys of `dict` into a caller-freed array; the array must be
 * free()d. Returns the count, or 0 (array left NULL) for an empty or
 * non-dictionary argument.
 */
static CFIndex
__copyDictKeys(CFDictionaryRef dict, const void ***keys)
{
	CFIndex	n;

	*keys = NULL;
	if ((dict == NULL) ||
	    (CFGetTypeID(dict) != CFDictionaryGetTypeID())) {
		return 0;
	}
	n = CFDictionaryGetCount(dict);
	if (n <= 0) {
		return 0;
	}
	*keys = (const void **)malloc((size_t)n * sizeof(void *));
	CFDictionaryGetKeysAndValues(dict, *keys, NULL);
	return n;
}


#pragma mark -
#pragma mark SCNetworkSet APIs

SCNetworkSetRef
SCNetworkSetCreate(SCPreferencesRef prefs)
{
	CFArrayRef		components;
	CFStringRef		path;
	CFStringRef		prefix;
	CFStringRef		setID;
	SCNetworkSetPrivateRef	sp;

	/* mint a unique /Sets/<setID> entry */
	prefix = CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@"),
					  kSCPrefSets);
	path = SCPreferencesPathCreateUniqueChild(prefs, prefix);
	CFRelease(prefix);
	if (path == NULL) {
		return NULL;
	}
	components = CFStringCreateArrayBySeparatingStrings(NULL, path,
							    CFSTR("/"));
	CFRelease(path);
	/* "/Sets/<id>" splits to ["", "Sets", id] */
	if ((components == NULL) || (CFArrayGetCount(components) < 3)) {
		if (components != NULL) CFRelease(components);
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}
	setID = CFArrayGetValueAtIndex(components, 2);
	sp = __SCNetworkSetCreatePrivate(NULL, prefs, setID);
	CFRelease(components);
	return (SCNetworkSetRef)sp;
}

SCNetworkSetRef
SCNetworkSetCopy(SCPreferencesRef prefs, CFStringRef setID)
{
	CFDictionaryRef		dict;
	CFStringRef		path;
	SCNetworkSetPrivateRef	sp;

	if ((setID == NULL) ||
	    (CFGetTypeID(setID) != CFStringGetTypeID())) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	path = __setPath(setID);
	dict = SCPreferencesPathGetValue(prefs, path);
	CFRelease(path);
	if ((dict == NULL) ||
	    (CFGetTypeID(dict) != CFDictionaryGetTypeID())) {
		_SCErrorSet(kSCStatusNoKey);
		return NULL;
	}
	sp = __SCNetworkSetCreatePrivate(NULL, prefs, setID);
	return (SCNetworkSetRef)sp;
}

CFArrayRef
SCNetworkSetCopyAll(SCPreferencesRef prefs)
{
	CFMutableArrayRef	array;
	const void		**keys;
	CFIndex			i, n;
	CFStringRef		path;
	CFDictionaryRef		sets;

	path = CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@"),
					kSCPrefSets);
	sets = SCPreferencesPathGetValue(prefs, path);
	CFRelease(path);
	if ((sets != NULL) &&
	    (CFGetTypeID(sets) != CFDictionaryGetTypeID())) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}

	array = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	n     = __copyDictKeys(sets, &keys);
	for (i = 0; i < n; i++) {
		SCNetworkSetPrivateRef	sp;

		sp = __SCNetworkSetCreatePrivate(NULL, prefs,
						 (CFStringRef)keys[i]);
		if (sp != NULL) {
			CFArrayAppendValue(array, sp);
			CFRelease(sp);
		}
	}
	if (keys != NULL)	free(keys);
	return array;
}

SCNetworkSetRef
SCNetworkSetCopyCurrent(SCPreferencesRef prefs)
{
	CFArrayRef	components;
	CFStringRef	currentID;
	SCNetworkSetRef	set	= NULL;

	currentID = SCPreferencesGetValue(prefs, kSCPrefCurrentSet);
	if ((currentID == NULL) ||
	    (CFGetTypeID(currentID) != CFStringGetTypeID())) {
		return NULL;
	}
	/* the value is the set's path "/Sets/<setID>" */
	components = CFStringCreateArrayBySeparatingStrings(NULL, currentID,
							    CFSTR("/"));
	if ((components != NULL) && (CFArrayGetCount(components) == 3)) {
		set = SCNetworkSetCopy(prefs,
				       CFArrayGetValueAtIndex(components, 2));
	}
	if (components != NULL) {
		CFRelease(components);
	}
	return set;
}

Boolean
SCNetworkSetSetCurrent(SCNetworkSetRef set)
{
	SCNetworkSetPrivateRef	sp	= (SCNetworkSetPrivateRef)set;
	CFStringRef		path;
	Boolean			ok;

	if (!isA_SCNetworkSet(set)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	path = __setPath(sp->setID);
	ok   = SCPreferencesSetValue(sp->prefs, kSCPrefCurrentSet, path);
	CFRelease(path);
	return ok;
}

CFStringRef
SCNetworkSetGetSetID(SCNetworkSetRef set)
{
	if (!isA_SCNetworkSet(set)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	return ((SCNetworkSetPrivateRef)set)->setID;
}

CFStringRef
SCNetworkSetGetName(SCNetworkSetRef set)
{
	SCNetworkSetPrivateRef	sp	= (SCNetworkSetPrivateRef)set;
	CFDictionaryRef		dict;
	CFStringRef		name;
	CFStringRef		path;

	if (!isA_SCNetworkSet(set)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	if (sp->name != NULL) {
		return sp->name;
	}
	path = __setPath(sp->setID);
	dict = SCPreferencesPathGetValue(sp->prefs, path);
	CFRelease(path);
	if ((dict != NULL) &&
	    (CFGetTypeID(dict) == CFDictionaryGetTypeID())) {
		name = CFDictionaryGetValue(dict, kSCPropUserDefinedName);
		if ((name != NULL) &&
		    (CFGetTypeID(name) == CFStringGetTypeID())) {
			sp->name = (CFStringRef)CFRetain(name);
			return sp->name;
		}
	}
	return NULL;
}

Boolean
SCNetworkSetSetName(SCNetworkSetRef set, CFStringRef name)
{
	SCNetworkSetPrivateRef	sp	= (SCNetworkSetPrivateRef)set;
	CFDictionaryRef		dict;
	CFMutableDictionaryRef	newDict;
	CFStringRef		path;
	Boolean			ok;

	if (!isA_SCNetworkSet(set)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if ((name != NULL) &&
	    (CFGetTypeID(name) != CFStringGetTypeID())) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	path = __setPath(sp->setID);
	dict = SCPreferencesPathGetValue(sp->prefs, path);
	if ((dict != NULL) &&
	    (CFGetTypeID(dict) == CFDictionaryGetTypeID())) {
		newDict = CFDictionaryCreateMutableCopy(NULL, 0, dict);
	} else {
		newDict = CFDictionaryCreateMutable(NULL, 0,
					&kCFTypeDictionaryKeyCallBacks,
					&kCFTypeDictionaryValueCallBacks);
	}
	if (name != NULL) {
		CFDictionarySetValue(newDict, kSCPropUserDefinedName, name);
	} else {
		CFDictionaryRemoveValue(newDict, kSCPropUserDefinedName);
	}

	ok = SCPreferencesPathSetValue(sp->prefs, path, newDict);
	CFRelease(newDict);
	CFRelease(path);

	if (ok) {
		if (sp->name != NULL) {
			CFRelease(sp->name);
		}
		sp->name = (name != NULL) ? CFStringCreateCopy(NULL, name)
					  : NULL;
	}
	return ok;
}

Boolean
SCNetworkSetRemove(SCNetworkSetRef set)
{
	SCNetworkSetPrivateRef	sp	= (SCNetworkSetPrivateRef)set;
	CFStringRef		currentPath;
	CFStringRef		path;
	Boolean			ok;

	if (!isA_SCNetworkSet(set)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	path = __setPath(sp->setID);

	/* the active set may not be removed */
	currentPath = SCPreferencesGetValue(sp->prefs, kSCPrefCurrentSet);
	if ((currentPath != NULL) &&
	    (CFGetTypeID(currentPath) == CFStringGetTypeID()) &&
	    CFEqual(currentPath, path)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		CFRelease(path);
		return FALSE;
	}

	ok = SCPreferencesPathRemoveValue(sp->prefs, path);
	CFRelease(path);
	return ok;
}


#pragma mark -
#pragma mark Services in a set

CFArrayRef
SCNetworkSetCopyServices(SCNetworkSetRef set)
{
	SCNetworkSetPrivateRef	sp	= (SCNetworkSetPrivateRef)set;
	CFMutableArrayRef	array;
	CFDictionaryRef		dict;
	const void		**keys;
	CFIndex			i, n;
	CFStringRef		path;

	if (!isA_SCNetworkSet(set)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	path = __setServicePath(sp->setID, NULL);
	dict = SCPreferencesPathGetValue(sp->prefs, path);
	CFRelease(path);
	if ((dict != NULL) &&
	    (CFGetTypeID(dict) != CFDictionaryGetTypeID())) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}

	array = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	n     = __copyDictKeys(dict, &keys);
	for (i = 0; i < n; i++) {
		SCNetworkServiceRef	service;

		/* each membership key is a serviceID; keep it only if the
		 * referenced service still exists */
		service = SCNetworkServiceCopy(sp->prefs,
					       (CFStringRef)keys[i]);
		if (service != NULL) {
			CFArrayAppendValue(array, service);
			CFRelease(service);
		}
	}
	if (keys != NULL)	free(keys);
	return array;
}

Boolean
SCNetworkSetAddService(SCNetworkSetRef set, SCNetworkServiceRef service)
{
	SCNetworkSetPrivateRef		sp	= (SCNetworkSetPrivateRef)set;
	SCNetworkServicePrivateRef	svcp	= (SCNetworkServicePrivateRef)service;
	CFStringRef			link;
	CFStringRef			path;
	Boolean				ok;

	if (!isA_SCNetworkSet(set)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if (!isA_SCNetworkService(service) || (svcp->prefs == NULL)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	/* link /Sets/<set>/Network/Service/<svc> -> /NetworkServices/<svc> */
	path = __setServicePath(sp->setID, svcp->serviceID);
	link = __netServicePath(svcp->serviceID);
	ok   = SCPreferencesPathSetLink(sp->prefs, path, link);
	CFRelease(path);
	CFRelease(link);
	return ok;
}

Boolean
SCNetworkSetRemoveService(SCNetworkSetRef set, SCNetworkServiceRef service)
{
	SCNetworkSetPrivateRef		sp	= (SCNetworkSetPrivateRef)set;
	SCNetworkServicePrivateRef	svcp	= (SCNetworkServicePrivateRef)service;
	CFStringRef			path;
	Boolean				ok;

	if (!isA_SCNetworkSet(set)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if (!isA_SCNetworkService(service)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	path = __setServicePath(sp->setID, svcp->serviceID);
	if (SCPreferencesPathGetLink(sp->prefs, path) == NULL) {
		/* the service is not a member of this set */
		_SCErrorSet(kSCStatusNoKey);
		CFRelease(path);
		return FALSE;
	}
	ok = SCPreferencesPathRemoveValue(sp->prefs, path);
	CFRelease(path);
	return ok;
}


#pragma mark -
#pragma mark Service order

CFArrayRef
SCNetworkSetGetServiceOrder(SCNetworkSetRef set)
{
	SCNetworkSetPrivateRef	sp	= (SCNetworkSetPrivateRef)set;
	CFDictionaryRef		dict;
	CFArrayRef		order;
	CFStringRef		path;

	if (!isA_SCNetworkSet(set)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	path = __setGlobalIPv4Path(sp->setID);
	dict = SCPreferencesPathGetValue(sp->prefs, path);
	CFRelease(path);
	if ((dict == NULL) ||
	    (CFGetTypeID(dict) != CFDictionaryGetTypeID())) {
		return NULL;
	}
	order = CFDictionaryGetValue(dict, kSCPropNetServiceOrder);
	if ((order == NULL) ||
	    (CFGetTypeID(order) != CFArrayGetTypeID())) {
		return NULL;
	}
	return order;
}

Boolean
SCNetworkSetSetServiceOrder(SCNetworkSetRef set, CFArrayRef newOrder)
{
	SCNetworkSetPrivateRef	sp	= (SCNetworkSetPrivateRef)set;
	CFDictionaryRef		dict;
	CFMutableDictionaryRef	newDict;
	CFIndex			i, n;
	CFStringRef		path;
	Boolean			ok;

	if (!isA_SCNetworkSet(set)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	if ((newOrder == NULL) ||
	    (CFGetTypeID(newOrder) != CFArrayGetTypeID())) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	n = CFArrayGetCount(newOrder);
	for (i = 0; i < n; i++) {
		CFTypeRef	sid = CFArrayGetValueAtIndex(newOrder, i);

		if (CFGetTypeID(sid) != CFStringGetTypeID()) {
			_SCErrorSet(kSCStatusInvalidArgument);
			return FALSE;
		}
	}

	path = __setGlobalIPv4Path(sp->setID);
	dict = SCPreferencesPathGetValue(sp->prefs, path);
	if ((dict != NULL) &&
	    (CFGetTypeID(dict) == CFDictionaryGetTypeID())) {
		newDict = CFDictionaryCreateMutableCopy(NULL, 0, dict);
	} else {
		newDict = CFDictionaryCreateMutable(NULL, 0,
					&kCFTypeDictionaryKeyCallBacks,
					&kCFTypeDictionaryValueCallBacks);
	}
	CFDictionarySetValue(newDict, kSCPropNetServiceOrder, newOrder);

	ok = SCPreferencesPathSetValue(sp->prefs, path, newDict);
	CFRelease(newDict);
	CFRelease(path);
	return ok;
}
