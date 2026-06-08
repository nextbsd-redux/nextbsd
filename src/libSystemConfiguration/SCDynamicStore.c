/*
 * SCDynamicStore.c — the SCDynamicStore session object and its
 * synchronous store API (freebsd-launchd-mach port of Apple's
 * SystemConfiguration.fproj SCDOpen.c / SCDGet.c / SCDSet.c /
 * SCDAdd.c / SCDRemove.c / SCDList.c).
 *
 * An SCDynamicStoreRef is a CoreFoundation runtime type. Creating one
 * looks configd's Mach service up via the bootstrap port and issues a
 * `configopen` — configd hands back a per-session Mach port, which is
 * the `server` argument every later config.defs RPC rides on. The
 * get / set / add / remove / list calls serialize CF objects to the
 * inline config.defs byte payloads (see SCD.c) and translate configd's
 * kSCStatus reply into SCError() / a Boolean or CFType result.
 *
 * Apple drives this through a per-object dispatch queue and a reconnect
 * path that re-opens the session if configd restarts. iter 1 keeps it
 * synchronous and single-shot: no queue, no reconnect. A caller that
 * outlives a configd restart re-creates its session — reconnect is a
 * later iteration, alongside change notifications.
 */

#include "SCInternal.h"
#include "config_wire.h"		/* wire_keylist_next — configlist payload */

#include <pthread.h>
#include <string.h>
#include <servers/bootstrap.h>


#pragma mark -
#pragma mark CoreFoundation runtime type

static CFTypeID		__kSCDynamicStoreTypeID	= _kCFRuntimeNotATypeID;

static void
__SCDynamicStoreDeallocate(CFTypeRef cf)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)cf;

	/* tear down any active dispatch-queue notification */
	__SCDynamicStoreNotifyCancel((SCDynamicStoreRef)cf);

	/*
	 * Drop the run-loop source. The notification thread is already
	 * stopped (it held a store reference, so we could not get here
	 * while it ran), hence the source is fully unscheduled and
	 * invalidating it calls no further callbacks.
	 */
	if (storePrivate->rls != NULL) {
		CFRunLoopSourceInvalidate(storePrivate->rls);
		CFRelease(storePrivate->rls);
		storePrivate->rls = NULL;
	}

	/*
	 * Drop our send right to the per-session port. configd has
	 * MACH_NOTIFY_NO_SENDERS armed on it, so this closes the
	 * session server-side.
	 */
	if (storePrivate->server != MACH_PORT_NULL) {
		(void) mach_port_deallocate(mach_task_self(), storePrivate->server);
		storePrivate->server = MACH_PORT_NULL;
	}

	/* release the callback context, if the client asked us to */
	if (storePrivate->rlsContext.release != NULL) {
		storePrivate->rlsContext.release(storePrivate->rlsContext.info);
	}

	if (storePrivate->name != NULL) {
		CFRelease(storePrivate->name);
	}
	if (storePrivate->options != NULL) {
		CFRelease(storePrivate->options);
	}
}

static CFStringRef
__SCDynamicStoreCopyDescription(CFTypeRef cf)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)cf;
	const char			*state;

	state = (storePrivate->server != MACH_PORT_NULL)
		    ? "<SCDynamicStore: connected>"
		    : "<SCDynamicStore: no server>";
	return CFStringCreateWithCString(NULL, state, kCFStringEncodingASCII);
}

static const CFRuntimeClass __SCDynamicStoreClass = {
	.version	= 0,
	.className	= "SCDynamicStore",
	.finalize	= __SCDynamicStoreDeallocate,
	.copyDebugDesc	= __SCDynamicStoreCopyDescription,
};

static void
__SCDynamicStoreClassInitialize(void)
{
	__kSCDynamicStoreTypeID = _CFRuntimeRegisterClass(&__SCDynamicStoreClass);
}

CFTypeID
SCDynamicStoreGetTypeID(void)
{
	static pthread_once_t	once	= PTHREAD_ONCE_INIT;

	(void) pthread_once(&once, __SCDynamicStoreClassInitialize);
	return __kSCDynamicStoreTypeID;
}

/* isA_SCDynamicStore() is a static inline in SCInternal.h */


#pragma mark -
#pragma mark Session establishment

static SCDynamicStorePrivateRef
__SCDynamicStoreCreatePrivate(CFAllocatorRef		allocator,
			      CFStringRef		name,
			      SCDynamicStoreCallBack	callout,
			      SCDynamicStoreContext	*context)
{
	SCDynamicStorePrivateRef	storePrivate;
	CFIndex				extra;

	/* extra bytes = everything past the leading CFRuntimeBase */
	extra = sizeof(SCDynamicStorePrivate) - sizeof(CFRuntimeBase);
	storePrivate = (SCDynamicStorePrivateRef)
		       _CFRuntimeCreateInstance(allocator,
						SCDynamicStoreGetTypeID(),
						extra,
						NULL);
	if (storePrivate == NULL) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}

	storePrivate->name	= (name != NULL) ? (CFStringRef)CFRetain(name) : NULL;
	storePrivate->options	= NULL;
	storePrivate->server	= MACH_PORT_NULL;
	storePrivate->notifyStatus	= NotifierNotRegistered;
	storePrivate->notifyPort	= MACH_PORT_NULL;
	storePrivate->notifySource	= NULL;
	storePrivate->notifyQueue	= NULL;
	storePrivate->dispatchQueue	= NULL;
	storePrivate->rls		= NULL;
	storePrivate->rlsRunLoop	= NULL;
	storePrivate->rlsScheduled	= 0;
	storePrivate->rlsFunction = callout;
	memset(&storePrivate->rlsContext, 0, sizeof(storePrivate->rlsContext));
	if (context != NULL) {
		memcpy(&storePrivate->rlsContext, context, sizeof(SCDynamicStoreContext));
		if (context->retain != NULL) {
			storePrivate->rlsContext.info =
				(void *)context->retain(context->info);
		}
	}

	return storePrivate;
}

/*
 * Look configd's Mach service up via the bootstrap port. Returns a
 * send right (caller deallocates) or MACH_PORT_NULL with *sc_status
 * set.
 */
static mach_port_t
__SCDynamicStoreServerPort(int *sc_status)
{
	mach_port_t	server	= MACH_PORT_NULL;
	kern_return_t	kr;

	kr = bootstrap_look_up(bootstrap_port, SCD_SERVER, &server);
	if (kr != BOOTSTRAP_SUCCESS) {
		*sc_status = kSCStatusNoStoreServer;
		return MACH_PORT_NULL;
	}
	return server;
}

/*
 * Issue the configopen that turns a freshly-allocated object into a
 * live session. On success storePrivate->server holds the per-session
 * port configd handed back.
 */
static Boolean
__SCDynamicStoreAddSession(SCDynamicStorePrivateRef storePrivate)
{
	CFDataRef	nameData	= NULL;
	CFDataRef	optionsData	= NULL;
	const uint8_t	*nameBytes	= (const uint8_t *)"";
	const uint8_t	*optionsBytes	= (const uint8_t *)"";
	mach_msg_type_number_t	nameLen		= 0;
	mach_msg_type_number_t	optionsLen	= 0;
	int		sc_status	= kSCStatusFailed;
	mach_port_t	server;
	kern_return_t	kr;

	/* serialize the session name */
	if (storePrivate->name != NULL) {
		nameData = _SCSerializeString(storePrivate->name);
		if (nameData == NULL) {
			_SCErrorSet(kSCStatusFailed);
			return FALSE;
		}
		nameBytes = CFDataGetBytePtr(nameData);
		nameLen   = (mach_msg_type_number_t)CFDataGetLength(nameData);
	}

	/* serialize the session options, if any */
	if (storePrivate->options != NULL) {
		optionsData = _SCSerialize(storePrivate->options);
		if (optionsData == NULL) {
			if (nameData != NULL) CFRelease(nameData);
			_SCErrorSet(kSCStatusFailed);
			return FALSE;
		}
		optionsBytes = CFDataGetBytePtr(optionsData);
		optionsLen   = (mach_msg_type_number_t)CFDataGetLength(optionsData);
	}

	if ((nameLen > CONFIG_DATA_MAX) || (optionsLen > CONFIG_DATA_MAX)) {
		sc_status = kSCStatusInvalidArgument;
		goto done;
	}

	server = __SCDynamicStoreServerPort(&sc_status);
	if (server == MACH_PORT_NULL) {
		goto done;
	}

	kr = configopen(server,
			(uint8_t *)nameBytes, nameLen,
			(uint8_t *)optionsBytes, optionsLen,
			&storePrivate->server,
			&sc_status);

	/* the looked-up service port is not needed past the open */
	(void) mach_port_deallocate(mach_task_self(), server);

	if (kr != KERN_SUCCESS) {
		sc_status = kSCStatusNoStoreServer;
	}

    done :

	if (nameData != NULL)    CFRelease(nameData);
	if (optionsData != NULL) CFRelease(optionsData);

	if (sc_status != kSCStatusOK) {
		_SCErrorSet(sc_status);
		return FALSE;
	}
	return TRUE;
}

SCDynamicStoreRef
SCDynamicStoreCreateWithOptions(CFAllocatorRef		allocator,
				CFStringRef		name,
				CFDictionaryRef		storeOptions,
				SCDynamicStoreCallBack	callout,
				SCDynamicStoreContext	*context)
{
	SCDynamicStorePrivateRef	storePrivate;

	storePrivate = __SCDynamicStoreCreatePrivate(allocator, name, callout, context);
	if (storePrivate == NULL) {
		return NULL;
	}

	if (storeOptions != NULL) {
		storePrivate->options = (CFDictionaryRef)CFRetain(storeOptions);
	}

	if (!__SCDynamicStoreAddSession(storePrivate)) {
		CFRelease(storePrivate);
		return NULL;
	}

	return (SCDynamicStoreRef)storePrivate;
}

SCDynamicStoreRef
SCDynamicStoreCreate(CFAllocatorRef		allocator,
		     CFStringRef		name,
		     SCDynamicStoreCallBack	callout,
		     SCDynamicStoreContext	*context)
{
	return SCDynamicStoreCreateWithOptions(allocator, name, NULL,
					       callout, context);
}


#pragma mark -
#pragma mark Store access

/*
 * Common front matter for every store call: validate the session and
 * serialize the key. Returns the serialized key (caller releases) or
 * NULL with SCError() already set.
 */
static CFDataRef
__SCStorePrepare(SCDynamicStoreRef store, CFStringRef key)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	CFDataRef			keyData;

	if (!isA_SCDynamicStore(store)) {
		_SCErrorSet(kSCStatusNoStoreSession);
		return NULL;
	}
	if (storePrivate->server == MACH_PORT_NULL) {
		_SCErrorSet(kSCStatusNoStoreServer);
		return NULL;
	}

	keyData = _SCSerializeString(key);
	if (keyData == NULL) {
		return NULL;
	}
	if (CFDataGetLength(keyData) > CONFIG_DATA_MAX) {
		CFRelease(keyData);
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	return keyData;
}

CFPropertyListRef
SCDynamicStoreCopyValue(SCDynamicStoreRef store, CFStringRef key)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	CFDataRef			keyData;
	CFPropertyListRef		value;
	xmlDataOut			reply;
	mach_msg_type_number_t		replyLen	= 0;
	int				newInstance	= 0;
	int				sc_status	= kSCStatusFailed;
	kern_return_t			kr;

	keyData = __SCStorePrepare(store, key);
	if (keyData == NULL) {
		return NULL;
	}

	kr = configget(storePrivate->server,
		       (uint8_t *)CFDataGetBytePtr(keyData),
		       (mach_msg_type_number_t)CFDataGetLength(keyData),
		       reply, &replyLen,
		       &newInstance, &sc_status);
	CFRelease(keyData);

	if (kr != KERN_SUCCESS) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}
	if (sc_status != kSCStatusOK) {
		_SCErrorSet(sc_status);
		return NULL;
	}

	value = _SCUnserialize(reply, replyLen);
	_SCErrorSet((value != NULL) ? kSCStatusOK : kSCStatusFailed);
	return value;
}

/*
 * Shared body of SCDynamicStoreSetValue / SCDynamicStoreAddValue: the
 * two differ only in which config.defs routine carries the key+value
 * (configset replaces, configadd refuses an existing key).
 */
static Boolean
__SCStoreWrite(SCDynamicStoreRef store, CFStringRef key,
	       CFPropertyListRef value, Boolean isAdd)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	CFDataRef			keyData;
	CFDataRef			valueData;
	int				newInstance	= 0;
	int				sc_status	= kSCStatusFailed;
	kern_return_t			kr;

	keyData = __SCStorePrepare(store, key);
	if (keyData == NULL) {
		return FALSE;
	}

	valueData = _SCSerialize(value);
	if (valueData == NULL) {
		CFRelease(keyData);
		return FALSE;
	}
	if (CFDataGetLength(valueData) > CONFIG_DATA_MAX) {
		CFRelease(keyData);
		CFRelease(valueData);
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	if (isAdd) {
		kr = configadd(storePrivate->server,
			       (uint8_t *)CFDataGetBytePtr(keyData),
			       (mach_msg_type_number_t)CFDataGetLength(keyData),
			       (uint8_t *)CFDataGetBytePtr(valueData),
			       (mach_msg_type_number_t)CFDataGetLength(valueData),
			       &newInstance, &sc_status);
	} else {
		kr = configset(storePrivate->server,
			       (uint8_t *)CFDataGetBytePtr(keyData),
			       (mach_msg_type_number_t)CFDataGetLength(keyData),
			       (uint8_t *)CFDataGetBytePtr(valueData),
			       (mach_msg_type_number_t)CFDataGetLength(valueData),
			       0, &newInstance, &sc_status);
	}

	CFRelease(keyData);
	CFRelease(valueData);

	if (kr != KERN_SUCCESS) {
		_SCErrorSet(kSCStatusFailed);
		return FALSE;
	}
	if (sc_status != kSCStatusOK) {
		_SCErrorSet(sc_status);
		return FALSE;
	}
	return TRUE;
}

Boolean
SCDynamicStoreSetValue(SCDynamicStoreRef store, CFStringRef key,
		       CFPropertyListRef value)
{
	return __SCStoreWrite(store, key, value, FALSE);
}

Boolean
SCDynamicStoreAddValue(SCDynamicStoreRef store, CFStringRef key,
		       CFPropertyListRef value)
{
	return __SCStoreWrite(store, key, value, TRUE);
}

Boolean
SCDynamicStoreRemoveValue(SCDynamicStoreRef store, CFStringRef key)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	CFDataRef			keyData;
	int				sc_status	= kSCStatusFailed;
	kern_return_t			kr;

	keyData = __SCStorePrepare(store, key);
	if (keyData == NULL) {
		return FALSE;
	}

	kr = configremove(storePrivate->server,
			  (uint8_t *)CFDataGetBytePtr(keyData),
			  (mach_msg_type_number_t)CFDataGetLength(keyData),
			  &sc_status);
	CFRelease(keyData);

	if (kr != KERN_SUCCESS) {
		_SCErrorSet(kSCStatusFailed);
		return FALSE;
	}
	if (sc_status != kSCStatusOK) {
		_SCErrorSet(sc_status);
		return FALSE;
	}
	return TRUE;
}

Boolean
SCDynamicStoreNotifyValue(SCDynamicStoreRef store, CFStringRef key)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	CFDataRef			keyData;
	int				sc_status	= kSCStatusFailed;
	kern_return_t			kr;

	keyData = __SCStorePrepare(store, key);
	if (keyData == NULL) {
		return FALSE;
	}

	/* confignotify posts a change for the key without altering it */
	kr = confignotify(storePrivate->server,
			  (uint8_t *)CFDataGetBytePtr(keyData),
			  (mach_msg_type_number_t)CFDataGetLength(keyData),
			  &sc_status);
	CFRelease(keyData);

	if (kr != KERN_SUCCESS) {
		_SCErrorSet(kSCStatusFailed);
		return FALSE;
	}
	if (sc_status != kSCStatusOK) {
		_SCErrorSet(sc_status);
		return FALSE;
	}
	return TRUE;
}

CFArrayRef
SCDynamicStoreCopyKeyList(SCDynamicStoreRef store, CFStringRef pattern)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	CFDataRef			patternData;
	CFMutableArrayRef		keys;
	xmlDataOut			reply;
	mach_msg_type_number_t		replyLen	= 0;
	int				sc_status	= kSCStatusFailed;
	kern_return_t			kr;
	const uint8_t			*cur;
	const uint8_t			*end;
	const void			*keyBytes;
	size_t				keyLen;

	patternData = __SCStorePrepare(store, pattern);
	if (patternData == NULL) {
		return NULL;
	}

	/* configd treats the argument as an anchored POSIX regex */
	kr = configlist(storePrivate->server,
			(uint8_t *)CFDataGetBytePtr(patternData),
			(mach_msg_type_number_t)CFDataGetLength(patternData),
			TRUE,
			reply, &replyLen,
			&sc_status);
	CFRelease(patternData);

	if (kr != KERN_SUCCESS) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}
	if (sc_status != kSCStatusOK) {
		_SCErrorSet(sc_status);
		return NULL;
	}

	/*
	 * configlist returns the key list as configd's wire form — a run
	 * of [uint32 LE len][key bytes] records. Decode it into a CFArray
	 * of CFString keys.
	 */
	keys = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	if (keys == NULL) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}

	cur = reply;
	end = reply + replyLen;
	while (wire_keylist_next(&cur, end, &keyBytes, &keyLen)) {
		CFStringRef	keyStr;

		keyStr = CFStringCreateWithBytes(NULL, keyBytes,
						 (CFIndex)keyLen,
						 kCFStringEncodingUTF8,
						 FALSE);
		if (keyStr != NULL) {
			CFArrayAppendValue(keys, keyStr);
			CFRelease(keyStr);
		}
	}

	_SCErrorSet(kSCStatusOK);
	return keys;
}
