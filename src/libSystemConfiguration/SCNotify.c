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
	mach_port_t			port		= MACH_PORT_NULL;
	kern_return_t			kr;

	(void)store;

	/*
	 * Allocate a receive right and insert a send right under the same
	 * name; the later notifyviaport (__sc_notify_register) hands that send
	 * right to configd. We keep the receive right to listen on. The
	 * notifyviaport is issued *after* the dispatch source is armed on this
	 * port (see __sc_notify_start) — telling configd to start sending
	 * before the edge-triggered EVFILT_MACHPORT knote is attached would
	 * race the first notification past it.
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

	return port;
}

/*
 * Hand configd the send right to notifyPort (notifyviaport) so it starts
 * posting change notifications. Call only after the MACH_RECV source is armed
 * on notifyPort. Returns TRUE, or FALSE with SCError() set.
 */
static Boolean
__sc_notify_register(SCDynamicStoreRef store)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	int				sc_status	= kSCStatusFailed;
	kern_return_t			kr;

	kr = notifyviaport(storePrivate->server, storePrivate->notifyPort, 0,
			   &sc_status);
	if ((kr != KERN_SUCCESS) || (sc_status != kSCStatusOK)) {
		_SCErrorSet((kr != KERN_SUCCESS) ? kSCStatusFailed : sc_status);
		return FALSE;
	}
	return TRUE;
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
 * MACH_RECV source event handler. configd posts a bare Mach message to
 * notifyPort whenever a watched key changes; the native EVFILT_MACHPORT
 * source wakes us here (#168/#250 — the task-#41 "doesn't reliably
 * deliver" was the retired module-era pipe bridge). The source registers
 * in notify mode and carries no message, so drain notifyPort ourselves —
 * fully, to empty: several configd notifications can coalesce into one fire,
 * and any message left queued keeps the source ready (it would re-fire, or
 * with an edge-triggered backend stall). The messages are bare headers, so
 * an unbounded drain can't be flooded. Then run the active delivery. Runs on
 * the caller's dispatch queue (dispatch mode) or the private notifyQueue
 * (run-loop mode).
 */
static void
__sc_source_handler(void *context)
{
	SCDynamicStoreRef		store		= (SCDynamicStoreRef)context;
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;

	for (;;) {
		struct {
			mach_msg_header_t	hdr;
			mach_msg_max_trailer_t	trailer;
		} rmsg;
		kern_return_t	kr;

		kr = mach_msg(&rmsg.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
			      sizeof(rmsg), storePrivate->notifyPort,
			      0 /* non-blocking */, MACH_PORT_NULL);
		if (kr != KERN_SUCCESS) {
			break;		/* drained (RCV_TIMED_OUT) or port error */
		}
	}

	if (storePrivate->notifyStatus == Using_NotifierInformViaDispatch) {
		/* the handler already runs on the caller's dispatchQueue */
		__SCDynamicStoreDeliverChanges(store);
	} else if (storePrivate->notifyStatus == Using_NotifierInformViaRunLoop) {
		/* signal the run-loop source and wake its run loop */
		CFRunLoopSourceSignal(storePrivate->rls);
		if (storePrivate->rlsRunLoop != NULL) {
			CFRunLoopWakeUp(storePrivate->rlsRunLoop);
		}
	}
}

/*
 * MACH_RECV source cancel handler. Runs asynchronously after the source is
 * cancelled and any in-flight event handler completes, so notifyPort and the
 * store stay valid until the source is truly done with them. Drops the
 * notifyPort receive right, the private queue (if any), and the source's
 * store reference (balancing the CFRetain in __sc_notify_start).
 */
static void
__sc_source_cancel(void *context)
{
	SCDynamicStoreRef		store		= (SCDynamicStoreRef)context;
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;

	if (storePrivate->notifyPort != MACH_PORT_NULL) {
		(void) mach_port_mod_refs(mach_task_self(),
					  storePrivate->notifyPort,
					  MACH_PORT_RIGHT_RECEIVE, -1);
		storePrivate->notifyPort = MACH_PORT_NULL;
	}
	if (storePrivate->notifyQueue != NULL) {
		dispatch_release(storePrivate->notifyQueue);
		storePrivate->notifyQueue = NULL;
	}
	CFRelease(store);
}

/*
 * Start notification delivery: open the notify port and attach a
 * DISPATCH_SOURCE_TYPE_MACH_RECV source. notifyStatus and any delivery-mode
 * fields must already be set. Returns TRUE, or FALSE with SCError() set and
 * the registration unwound.
 */
static Boolean
__sc_notify_start(SCDynamicStoreRef store)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	mach_port_t			mp;
	dispatch_queue_t		q;

	mp = __SCDynamicStoreAddNotificationPort(store);
	if (mp == MACH_PORT_NULL) {
		return FALSE;		/* SCError already set */
	}
	storePrivate->notifyPort = mp;

	/*
	 * The source runs on the caller's dispatch queue when delivering via
	 * dispatch; the run-loop path has no caller queue, so spin up a private
	 * serial queue just to drive the receive + run-loop signal.
	 */
	if (storePrivate->notifyStatus == Using_NotifierInformViaDispatch) {
		q = storePrivate->dispatchQueue;
	} else {
		storePrivate->notifyQueue =
		    dispatch_queue_create("com.apple.SCDynamicStore.notify", NULL);
		q = storePrivate->notifyQueue;
	}

	if (q != NULL) {
		storePrivate->notifySource =
		    dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV,
					   mp, 0, q);
	}
	if (q == NULL || storePrivate->notifySource == NULL) {
		int	sc_status;

		if (storePrivate->notifyQueue != NULL) {
			dispatch_release(storePrivate->notifyQueue);
			storePrivate->notifyQueue = NULL;
		}
		if (storePrivate->server != MACH_PORT_NULL) {
			(void) notifycancel(storePrivate->server, &sc_status);
		}
		(void) mach_port_mod_refs(mach_task_self(), mp,
					  MACH_PORT_RIGHT_RECEIVE, -1);
		storePrivate->notifyPort = MACH_PORT_NULL;
		_SCErrorSet(kSCStatusFailed);
		return FALSE;
	}

	/* the source holds a reference to the store for its lifetime */
	CFRetain(store);
	dispatch_set_context(storePrivate->notifySource, (void *)store);
	dispatch_source_set_event_handler_f(storePrivate->notifySource,
					    __sc_source_handler);
	dispatch_source_set_cancel_handler_f(storePrivate->notifySource,
					     __sc_source_cancel);
	dispatch_resume(storePrivate->notifySource);

	/*
	 * Arm the source BEFORE telling configd to start sending. The native
	 * EVFILT_MACHPORT filter is edge-triggered (EV_CLEAR) and libdispatch
	 * installs the knote asynchronously on its manager, so notifyviaport
	 * must not precede the arm — otherwise configd's first notification can
	 * reach notifyPort before the knote attaches and be missed (the bug the
	 * old blocking-mach_msg loop never had). The notifyviaport RPC also
	 * blocks this thread, which lets the manager complete the deferred knote
	 * install before configd even learns the port, closing the window.
	 */
	if (!__sc_notify_register(store)) {
		/*
		 * Cancel the source; its cancel handler (async) drops the
		 * notifyPort right, releases the queue, and releases the store
		 * reference taken above. The caller unwinds notifyStatus.
		 */
		dispatch_source_cancel(storePrivate->notifySource);
		dispatch_release(storePrivate->notifySource);
		storePrivate->notifySource = NULL;
		return FALSE;
	}

	/*
	 * Belt-and-suspenders: kick one drain+deliver on the source's queue in
	 * case a notification still slipped into the arm window. It is
	 * idempotent with the armed source — whichever drains first wins; the
	 * other finds the port empty and notifychanges returns nothing.
	 */
	dispatch_async_f(q, (void *)store, __sc_source_handler);
	return TRUE;
}

/*
 * Stop notification delivery. Cancels the MACH_RECV source; the cancel
 * handler (async, after any in-flight event handler) drops the notifyPort
 * receive right, releases the private queue, and releases the source's store
 * reference — so the port and store stay valid until the source is truly
 * done. We drop only our own source ref here.
 */
static void
__sc_notify_stop(SCDynamicStoreRef store)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	int				sc_status;

	if (storePrivate->server != MACH_PORT_NULL) {
		(void) notifycancel(storePrivate->server, &sc_status);
	}

	if (storePrivate->notifySource != NULL) {
		dispatch_source_cancel(storePrivate->notifySource);
		dispatch_release(storePrivate->notifySource);
		storePrivate->notifySource = NULL;
	}
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
