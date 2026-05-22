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

/*
 * dispatch_async_f trampoline — drains the changed keys and runs the
 * client callout. Runs on the caller's dispatch queue; balances the
 * CFRetain the receive thread took before queueing it.
 */
static void
__sc_deliver(void *context)
{
	SCDynamicStoreRef	store	= (SCDynamicStoreRef)context;

	__SCDynamicStoreDeliverChanges(store);
	CFRelease(store);
}

/*
 * Notification receive loop. configd posts a bare Mach message to
 * notifyPort whenever a watched key changes; this thread receives it
 * and hands delivery to the caller's dispatch queue. The receive has a
 * bounded timeout so the thread can observe notifyStop and exit when
 * the notification is cancelled.
 *
 * A raw mach_msg loop, not a DISPATCH_SOURCE_TYPE_MACH_RECV source:
 * dispatch mach-receive sources do not reliably deliver in this repo
 * (task #41) — hwregd's Mach service thread uses a raw loop for the
 * same reason.
 */
static void *
__sc_notify_thread(void *arg)
{
	SCDynamicStoreRef		store		= (SCDynamicStoreRef)arg;
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;

	while (!storePrivate->notifyStop) {
		struct {
			mach_msg_header_t	hdr;
			mach_msg_max_trailer_t	trailer;
		} rmsg;
		kern_return_t	kr;

		kr = mach_msg(&rmsg.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
			      sizeof(rmsg), storePrivate->notifyPort,
			      500 /* ms */, MACH_PORT_NULL);
		if (kr == MACH_RCV_TIMED_OUT) {
			continue;		/* re-check notifyStop */
		}
		if (kr != KERN_SUCCESS) {
			break;			/* port gone / unexpected error */
		}

		/* a notification arrived — deliver on the caller's queue */
		CFRetain(store);
		dispatch_async_f(storePrivate->dispatchQueue,
				 (void *)store, __sc_deliver);
	}

	return NULL;
}

Boolean
SCDynamicStoreSetDispatchQueue(SCDynamicStoreRef store, dispatch_queue_t queue)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	mach_port_t			mp;
	int				err;

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

	storePrivate->notifyPort	= mp;
	storePrivate->dispatchQueue	= queue;
	dispatch_retain(queue);
	storePrivate->notifyStop	= 0;
	storePrivate->notifyStatus	= Using_NotifierInformViaDispatch;

	/* the receive thread holds a reference to the store */
	CFRetain(store);

	err = pthread_create(&storePrivate->notifyThread, NULL,
			     __sc_notify_thread, (void *)store);
	if (err != 0) {
		/* unwind the registration */
		int	sc_status;

		CFRelease(store);
		dispatch_release(queue);
		storePrivate->dispatchQueue = NULL;
		storePrivate->notifyStatus  = NotifierNotRegistered;
		if (storePrivate->server != MACH_PORT_NULL) {
			(void) notifycancel(storePrivate->server, &sc_status);
		}
		(void) mach_port_mod_refs(mach_task_self(), mp,
					  MACH_PORT_RIGHT_RECEIVE, -1);
		storePrivate->notifyPort = MACH_PORT_NULL;
		_SCErrorSet(kSCStatusFailed);
		return FALSE;
	}

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

	/* stop the receive loop and wait for the thread to exit */
	storePrivate->notifyStop = 1;
	(void) pthread_join(storePrivate->notifyThread, NULL);

	/* ask configd to stop notifying this session */
	if (storePrivate->server != MACH_PORT_NULL) {
		(void) notifycancel(storePrivate->server, &sc_status);
	}

	/* drop the notification port's receive right */
	if (storePrivate->notifyPort != MACH_PORT_NULL) {
		(void) mach_port_mod_refs(mach_task_self(),
					  storePrivate->notifyPort,
					  MACH_PORT_RIGHT_RECEIVE, -1);
		storePrivate->notifyPort = MACH_PORT_NULL;
	}

	if (storePrivate->dispatchQueue != NULL) {
		dispatch_release(storePrivate->dispatchQueue);
		storePrivate->dispatchQueue = NULL;
	}

	storePrivate->notifyStatus = NotifierNotRegistered;

	/*
	 * Release the receive thread's store reference. Any callout still
	 * queued on the caller's queue holds its own reference (taken
	 * before dispatch_async_f), so the store stays alive until those
	 * drain.
	 */
	CFRelease(store);
}
