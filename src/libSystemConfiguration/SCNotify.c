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

	if ((_SCEncodeKeyList(keys, keyBuf, sizeof(keyBuf), &keyLen) != 0) ||
	    (_SCEncodeKeyList(patterns, patternBuf, sizeof(patternBuf), &patternLen) != 0)) {
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
	    (storePrivate->notifyStatus != NotifierNotRegistered)) {
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

		/* a notification arrived — hand it to the active delivery */
		if (storePrivate->notifyStatus == Using_NotifierInformViaDispatch) {
			CFRetain(store);
			dispatch_async_f(storePrivate->dispatchQueue,
					 (void *)store, __sc_deliver);
		} else if (storePrivate->notifyStatus == Using_NotifierInformViaRunLoop) {
			/* signal the run-loop source and wake its run loop */
			CFRunLoopSourceSignal(storePrivate->rls);
			if (storePrivate->rlsRunLoop != NULL) {
				CFRunLoopWakeUp(storePrivate->rlsRunLoop);
			}
		}
	}

	return NULL;
}

/*
 * Start the notification receive thread. notifyStatus and any
 * delivery-mode fields must already be set. Returns TRUE, or FALSE
 * with SCError() set and the registration unwound.
 */
static Boolean
__sc_notify_start(SCDynamicStoreRef store)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	mach_port_t			mp;
	int				err;

	mp = __SCDynamicStoreAddNotificationPort(store);
	if (mp == MACH_PORT_NULL) {
		return FALSE;		/* SCError already set */
	}
	storePrivate->notifyPort = mp;
	storePrivate->notifyStop = 0;

	/* the receive thread holds a reference to the store */
	CFRetain(store);

	err = pthread_create(&storePrivate->notifyThread, NULL,
			     __sc_notify_thread, (void *)store);
	if (err != 0) {
		int	sc_status;

		CFRelease(store);
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

/*
 * Stop the receive thread and tear the notification port down. Drops
 * the receive thread's store reference last; callers must not touch
 * storePrivate after this returns.
 */
static void
__sc_notify_stop(SCDynamicStoreRef store)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	int				sc_status;

	storePrivate->notifyStop = 1;
	(void) pthread_join(storePrivate->notifyThread, NULL);

	if (storePrivate->server != MACH_PORT_NULL) {
		(void) notifycancel(storePrivate->server, &sc_status);
	}
	if (storePrivate->notifyPort != MACH_PORT_NULL) {
		(void) mach_port_mod_refs(mach_task_self(),
					  storePrivate->notifyPort,
					  MACH_PORT_RIGHT_RECEIVE, -1);
		storePrivate->notifyPort = MACH_PORT_NULL;
	}

	/* release the receive thread's store reference */
	CFRelease(store);
}

Boolean
SCDynamicStoreSetDispatchQueue(SCDynamicStoreRef store, dispatch_queue_t queue)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;

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

	storePrivate->dispatchQueue = queue;
	dispatch_retain(queue);
	storePrivate->notifyStatus = Using_NotifierInformViaDispatch;

	if (!__sc_notify_start(store)) {
		dispatch_release(queue);
		storePrivate->dispatchQueue = NULL;
		storePrivate->notifyStatus  = NotifierNotRegistered;
		return FALSE;
	}
	return TRUE;
}

void
__SCDynamicStoreNotifyCancel(SCDynamicStoreRef store)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	dispatch_queue_t		queue;

	/* this is the dispatch-queue teardown; the run-loop source path
	   tears down through rlsCancel */
	if (storePrivate->notifyStatus != Using_NotifierInformViaDispatch) {
		return;
	}

	queue = storePrivate->dispatchQueue;
	storePrivate->dispatchQueue = NULL;
	storePrivate->notifyStatus  = NotifierNotRegistered;

	__sc_notify_stop(store);	/* last — drops the store reference */

	if (queue != NULL) {
		dispatch_release(queue);
	}
}


#pragma mark -
#pragma mark Run-loop-source delivery

/* CFRunLoopSource perform — runs the callout on the run loop thread */
static void
rlsPerform(void *info)
{
	__SCDynamicStoreDeliverChanges((SCDynamicStoreRef)info);
}

/*
 * CFRunLoopSource schedule — called when the source is added to a run
 * loop. On the first run loop it registers the notification port and
 * starts the receive thread.
 */
static void
rlsSchedule(void *info, CFRunLoopRef rl, CFRunLoopMode mode)
{
	SCDynamicStoreRef		store		= (SCDynamicStoreRef)info;
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;

	(void)mode;

	/* the receive thread wakes this run loop on a change. If the
	   source is added to several run loops only the last is woken
	   explicitly; CFRunLoopSourceSignal still marks it for them all. */
	storePrivate->rlsRunLoop = rl;

	if (storePrivate->rlsScheduled == 0) {
		if (storePrivate->server == MACH_PORT_NULL) {
			return;
		}
		storePrivate->notifyStatus = Using_NotifierInformViaRunLoop;
		if (!__sc_notify_start(store)) {
			storePrivate->notifyStatus = NotifierNotRegistered;
			return;
		}
	}
	storePrivate->rlsScheduled++;
}

/*
 * CFRunLoopSource cancel — called when the source is removed from a
 * run loop (or invalidated). Stops notifications once the source is no
 * longer scheduled anywhere.
 */
static void
rlsCancel(void *info, CFRunLoopRef rl, CFRunLoopMode mode)
{
	SCDynamicStoreRef		store		= (SCDynamicStoreRef)info;
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;

	(void)rl;
	(void)mode;

	if (storePrivate->rlsScheduled == 0) {
		return;
	}
	storePrivate->rlsScheduled--;
	if (storePrivate->rlsScheduled > 0) {
		return;
	}
	if (storePrivate->notifyStatus == Using_NotifierInformViaRunLoop) {
		storePrivate->notifyStatus = NotifierNotRegistered;
		storePrivate->rlsRunLoop   = NULL;
		__sc_notify_stop(store);	/* last — drops the store reference */
	}
}

CFRunLoopSourceRef
SCDynamicStoreCreateRunLoopSource(CFAllocatorRef	allocator,
				  SCDynamicStoreRef	store,
				  CFIndex		order)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	CFRunLoopSourceContext		context		= {
		.version	= 0,
		.info		= (void *)store,
		/*
		 * No retain/release on info: the store owns the source via
		 * storePrivate->rls, so a retaining context would form a
		 * reference cycle.
		 */
		.schedule	= rlsSchedule,
		.cancel		= rlsCancel,
		.perform	= rlsPerform,
	};

	if (!isA_SCDynamicStore(store)) {
		_SCErrorSet(kSCStatusNoStoreSession);
		return NULL;
	}
	if (storePrivate->server == MACH_PORT_NULL) {
		_SCErrorSet(kSCStatusNoStoreServer);
		return NULL;
	}

	/* one source per session — hand back the existing one, retained */
	if (storePrivate->rls != NULL) {
		CFRetain(storePrivate->rls);
		return storePrivate->rls;
	}

	storePrivate->rls = CFRunLoopSourceCreate(allocator, order, &context);
	if (storePrivate->rls == NULL) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}

	/* one reference for storePrivate->rls, one returned to the caller */
	CFRetain(storePrivate->rls);
	return storePrivate->rls;
}
