/*
 * SCNotify.c — SCDynamicStore change notifications (freebsd-launchd-mach
 * port of Apple's SystemConfiguration.fproj SCDNotifierSetKeys.c /
 * SCDNotifierGetChanges.c / SCDNotifierInformViaCallback.c).
 *
 * A session asks configd to watch a set of keys/patterns
 * (SCDynamicStoreSetNotificationKeys -> notifyset), and arranges for
 * delivery. configd notifies a watching session by sending a bare
 * Mach message to a port the session registered (notifyviaport); the
 * session then drains the changed-key list (notifychanges).
 *
 * Delivery mechanism — SCDynamicStoreSetDispatchQueue: Apple also
 * offers SCDynamicStoreCreateRunLoopSource, built on CFMachPort. This
 * repo's libCoreFoundation does not compile CFMachPort (it is gated to
 * TARGET_OS_MAC), so the run-loop-source variant is a later iteration.
 * The dispatch-queue path uses a DISPATCH_SOURCE_TYPE_MACH_RECV source
 * — which this repo's libdispatch implements and launchd / libxpc /
 * libnotify already rely on — so it is the iter-2 delivery path, and
 * ports nearly verbatim from Apple's SCDynamicStoreSetDispatchQueue.
 *
 * The watch set (notifyset) and the changed-key list (notifychanges)
 * both cross the wire as configd's length-prefixed key-list encoding
 * (config_wire.h), not as serialized property lists.
 */

#include "SCInternal.h"
#include "config_wire.h"

#include <dispatch/dispatch.h>


#pragma mark -
#pragma mark Watch set / changed-key list

/*
 * Encode a CFArray of CFString keys into configd's wire key-list form
 * ([uint32 LE len][bytes] records). Writes into buf (capacity cap) and
 * stores the byte count in *outLen. Returns 0, or -1 on a bad array
 * element or if the records do not fit.
 */
static int
encode_keylist(CFArrayRef array, uint8_t *buf, size_t cap, size_t *outLen)
{
	CFIndex	i;
	CFIndex	n;
	size_t	off	= 0;

	*outLen = 0;
	if (array == NULL) {
		return 0;
	}
	if (CFGetTypeID(array) != CFArrayGetTypeID()) {
		return -1;
	}

	n = CFArrayGetCount(array);
	for (i = 0; i < n; i++) {
		CFStringRef	key	= CFArrayGetValueAtIndex(array, i);
		CFDataRef	keyData;
		int		rc;

		if ((key == NULL) || (CFGetTypeID(key) != CFStringGetTypeID())) {
			return -1;
		}
		keyData = CFStringCreateExternalRepresentation(NULL, key,
							       kCFStringEncodingUTF8,
							       0);
		if (keyData == NULL) {
			return -1;
		}
		rc = wire_keylist_put(buf, cap, &off,
				      CFDataGetBytePtr(keyData),
				      (size_t)CFDataGetLength(keyData));
		CFRelease(keyData);
		if (rc != 0) {
			return -1;
		}
	}

	*outLen = off;
	return 0;
}

Boolean
SCDynamicStoreSetNotificationKeys(SCDynamicStoreRef	store,
				  CFArrayRef		keys,
				  CFArrayRef		patterns)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	uint8_t				keyBuf[CONFIG_DATA_MAX];
	uint8_t				patternBuf[CONFIG_DATA_MAX];
	size_t				keyLen		= 0;
	size_t				patternLen	= 0;
	int				sc_status	= kSCStatusFailed;
	kern_return_t			kr;

	if (!isA_SCDynamicStore(store)) {
		_SCErrorSet(kSCStatusNoStoreSession);
		return FALSE;
	}
	if (storePrivate->server == MACH_PORT_NULL) {
		_SCErrorSet(kSCStatusNoStoreServer);
		return FALSE;
	}

	if ((encode_keylist(keys, keyBuf, sizeof(keyBuf), &keyLen) != 0) ||
	    (encode_keylist(patterns, patternBuf, sizeof(patternBuf), &patternLen) != 0)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	kr = notifyset(storePrivate->server,
		       keyBuf, (mach_msg_type_number_t)keyLen,
		       patternBuf, (mach_msg_type_number_t)patternLen,
		       &sc_status);

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
SCDynamicStoreCopyNotifiedKeys(SCDynamicStoreRef store)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	xmlDataOut			reply;
	mach_msg_type_number_t		replyLen	= 0;
	int				sc_status	= kSCStatusFailed;
	kern_return_t			kr;
	CFMutableArrayRef		keys;
	const uint8_t			*cur;
	const uint8_t			*end;
	const void			*keyBytes;
	size_t				keyLen;

	if (!isA_SCDynamicStore(store)) {
		_SCErrorSet(kSCStatusNoStoreSession);
		return NULL;
	}
	if (storePrivate->server == MACH_PORT_NULL) {
		_SCErrorSet(kSCStatusNoStoreServer);
		return NULL;
	}

	kr = notifychanges(storePrivate->server, reply, &replyLen, &sc_status);
	if (kr != KERN_SUCCESS) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}
	if (sc_status != kSCStatusOK) {
		_SCErrorSet(sc_status);
		return NULL;
	}

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


#pragma mark -
#pragma mark Notification port

mach_port_t
__SCDynamicStoreAddNotificationPort(SCDynamicStoreRef store)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	mach_port_t			port		= MACH_PORT_NULL;
	int				sc_status	= kSCStatusFailed;
	kern_return_t			kr;

	/*
	 * Allocate a receive right and insert a send right under the same
	 * name; notifyviaport's mach_port_move_send_t hands that send
	 * right to configd. We keep the receive right to listen on.
	 */
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
	if (kr != KERN_SUCCESS) {
		_SCErrorSet(kSCStatusFailed);
		return MACH_PORT_NULL;
	}
	kr = mach_port_insert_right(mach_task_self(), port, port,
				    MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		(void) mach_port_mod_refs(mach_task_self(), port,
					  MACH_PORT_RIGHT_RECEIVE, -1);
		_SCErrorSet(kSCStatusFailed);
		return MACH_PORT_NULL;
	}

	kr = notifyviaport(storePrivate->server, port, 0, &sc_status);
	if ((kr != KERN_SUCCESS) || (sc_status != kSCStatusOK)) {
		(void) mach_port_mod_refs(mach_task_self(), port,
					  MACH_PORT_RIGHT_RECEIVE, -1);
		_SCErrorSet((kr != KERN_SUCCESS) ? kSCStatusFailed : sc_status);
		return MACH_PORT_NULL;
	}

	return port;
}


#pragma mark -
#pragma mark Dispatch-queue delivery

/*
 * Drain the changed-key list and run the client's callout. Apple's
 * rlsPerform(); runs on the caller's dispatch queue.
 */
static void
__SCDynamicStoreDeliverChanges(SCDynamicStoreRef store)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	SCDynamicStoreCallBack		callout;
	CFArrayRef			changedKeys;
	void				*info;
	void				(*info_release)(const void *);

	changedKeys = SCDynamicStoreCopyNotifiedKeys(store);
	if (changedKeys == NULL) {
		return;
	}
	if (CFArrayGetCount(changedKeys) == 0) {
		/* a notification with nothing pending — nothing to report */
		CFRelease(changedKeys);
		return;
	}

	callout = storePrivate->rlsFunction;

	/* hold the context info across the callout, if asked to */
	if (storePrivate->rlsContext.retain != NULL) {
		info = (void *)storePrivate->rlsContext.retain(storePrivate->rlsContext.info);
		info_release = storePrivate->rlsContext.release;
	} else {
		info = storePrivate->rlsContext.info;
		info_release = NULL;
	}

	if ((callout != NULL) &&
	    (storePrivate->notifyStatus == Using_NotifierInformViaDispatch)) {
		(*callout)(store, changedKeys, info);
	}

	if (info_release != NULL) {
		info_release(info);
	}
	CFRelease(changedKeys);
}

/* dispatch source finalizer — drop the store reference the source held */
static void
__sc_source_finalize(void *context)
{
	if (context != NULL) {
		CFRelease((CFTypeRef)context);
	}
}

Boolean
SCDynamicStoreSetDispatchQueue(SCDynamicStoreRef store, dispatch_queue_t queue)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	dispatch_queue_t		notifyQueue;
	dispatch_source_t		source;
	mach_port_t			mp;

	if (!isA_SCDynamicStore(store)) {
		_SCErrorSet(kSCStatusNoStoreSession);
		return FALSE;
	}

	if (queue == NULL) {
		/* unschedule */
		if (storePrivate->notifyStatus != Using_NotifierInformViaDispatch) {
			_SCErrorSet(kSCStatusInvalidArgument);
			return FALSE;
		}
		__SCDynamicStoreNotifyCancel(store);
		return TRUE;
	}

	if (storePrivate->server == MACH_PORT_NULL) {
		_SCErrorSet(kSCStatusNoStoreServer);
		return FALSE;
	}
	if (storePrivate->notifyStatus != NotifierNotRegistered) {
		/* only one notification registration at a time */
		_SCErrorSet(kSCStatusNotifierActive);
		return FALSE;
	}

	/* register a notification port with configd */
	mp = __SCDynamicStoreAddNotificationPort(store);
	if (mp == MACH_PORT_NULL) {
		return FALSE;		/* SCError already set */
	}

	storePrivate->notifyStatus	= Using_NotifierInformViaDispatch;
	storePrivate->notifyPort	= mp;

	/* the caller's queue — the callout runs here. Retained for the
	   storePrivate reference; released by __SCDynamicStoreNotifyCancel. */
	storePrivate->dispatchQueue = queue;
	dispatch_retain(storePrivate->dispatchQueue);

	/* a private serial queue drives the notification-port source */
	notifyQueue = dispatch_queue_create("SCDynamicStore notifications", NULL);

	source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV,
					mp, 0, notifyQueue);
	if (source == NULL) {
		dispatch_release(notifyQueue);
		__SCDynamicStoreNotifyCancel(store);
		_SCErrorSet(kSCStatusFailed);
		return FALSE;
	}

	/* the source keeps a reference to the store while it is active */
	CFRetain(store);
	dispatch_set_context(source, (void *)store);
	dispatch_set_finalizer_f(source, __sc_source_finalize);

	/* a second reference to the caller's queue, for the event handler;
	   released by the cancel handler below */
	dispatch_retain(queue);

	dispatch_source_set_event_handler(source, ^{
		struct {
			mach_msg_header_t	hdr;
			mach_msg_max_trailer_t	trailer;
		} rmsg;
		kern_return_t	kr;

		/* drain configd's bare notification message off the port */
		kr = mach_msg(&rmsg.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
			      sizeof(rmsg), mp, 0, MACH_PORT_NULL);
		if (kr != KERN_SUCCESS) {
			return;
		}

		/* run the callout on the caller's queue */
		CFRetain(store);
		dispatch_async(queue, ^{
			__SCDynamicStoreDeliverChanges(store);
			CFRelease(store);
		});
	});

	dispatch_source_set_cancel_handler(source, ^{
		/* drop the notification port's receive right */
		(void) mach_port_mod_refs(mach_task_self(), mp,
					  MACH_PORT_RIGHT_RECEIVE, -1);
		dispatch_release(notifyQueue);
		dispatch_release(source);
		dispatch_release(queue);
	});

	storePrivate->dispatchSource = source;
	dispatch_resume(source);
	return TRUE;
}

void
__SCDynamicStoreNotifyCancel(SCDynamicStoreRef store)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	int				sc_status;

	if (storePrivate->notifyStatus != Using_NotifierInformViaDispatch) {
		return;
	}

	/* ask configd to stop notifying this session */
	if (storePrivate->server != MACH_PORT_NULL) {
		(void) notifycancel(storePrivate->server, &sc_status);
	}

	/*
	 * Cancelling the source runs its cancel handler on the source's
	 * private queue: that removes the notification port and releases
	 * the source, its private queue, and the event handler's queue
	 * reference. The source finalizer then drops the store reference.
	 */
	if (storePrivate->dispatchSource != NULL) {
		dispatch_source_cancel(storePrivate->dispatchSource);
		storePrivate->dispatchSource = NULL;
	}
	if (storePrivate->dispatchQueue != NULL) {
		dispatch_release(storePrivate->dispatchQueue);
		storePrivate->dispatchQueue = NULL;
	}
	storePrivate->notifyPort	= MACH_PORT_NULL;
	storePrivate->notifyStatus	= NotifierNotRegistered;
}
