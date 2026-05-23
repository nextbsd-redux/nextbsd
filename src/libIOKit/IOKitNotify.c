/*
 * IOKitNotify.c — libIOKit iter 4: K2 device-arrival / departure
 * notifications.
 *
 * Each IONotificationPort owns one Mach receive right and one
 * private pthread that drives a raw mach_msg(MACH_RCV_MSG|MACH_RCV_
 * TIMEOUT, 500ms) loop on it. Watch registrations go through
 * hwreg_watch (which targets the IONotificationPort's recv port);
 * events fan in on the one port and the receive thread re-matches
 * each event's device fields against the per-watch criteria stored
 * client-side, then fires each matching IOServiceMatchingCallback
 * (via dispatch_async_f if a queue is set, inline on the receive
 * thread otherwise).
 *
 * Why raw mach_msg and not a libdispatch MACH_RECV source: task
 * #41 + the libSystemConfiguration iter-2 / hwregd-Phase-0 notes
 * confirm DISPATCH_SOURCE_TYPE_MACH_RECV does not reliably deliver
 * in this repo. hwregd, libSystemConfiguration's SCNotify and now
 * this facade all use the same pthread+timed-mach_msg pattern.
 */
#include <IOKit/IOKitLib.h>
#include "IOKitInternal.h"

#include "hwreg.h"
#include "hwreg_mig_types.h"

#include <dispatch/dispatch.h>
#include <mach/mach.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The hwregd-side watch carried as one IOServiceAddMatching
 * Notification. The criteria fields are kept client-side so that
 * when multiple watches share one IONotificationPort, the receive
 * thread can re-match each event against them and fire the right
 * callback(s).
 */
struct __io_watch {
	struct __io_watch		*next;
	uint64_t			 watcher_id;
	uint32_t			 event_mask;	/* HWREG_EVT_* */
	struct io_criteria		 criteria;
	IOServiceMatchingCallback	 callback;
	void				*refcon;
};

struct IONotificationPort {
	mach_port_t		 recv_port;	/* receive right we own */
	pthread_t		 thread;
	atomic_int		 stop;
	pthread_mutex_t		 lock;		/* guards `watches` + queue */
	struct __io_watch	*watches;
	dispatch_queue_t	 queue;		/* optional callback target */
};

/*
 * Bundle passed to dispatch_async_f so the receive thread can hand
 * the callback off to a user dispatch queue. The bundle owns the
 * io_iterator_t (allocated by us, released by IOObjectRelease in
 * the dispatch trampoline after the user callback returns).
 */
struct callout_bundle {
	IOServiceMatchingCallback	 callback;
	void				*refcon;
	io_iterator_t			 iterator;
};

static void
callout_trampoline(void *ctx)
{
	struct callout_bundle *b = ctx;

	b->callback(b->refcon, b->iterator);
	IOObjectRelease(b->iterator);
	free(b);
}

static void
fire_callback(struct IONotificationPort *port, struct __io_watch *w,
    uint64_t node_id)
{
	uint64_t *ids = calloc(1, sizeof(*ids));
	io_iterator_t it;
	dispatch_queue_t q;

	if (ids == NULL)
		return;
	ids[0] = node_id;
	it = __io_alloc_iterator(ids, 1);	/* takes ownership of ids */
	if (it == IO_OBJECT_NULL)
		return;

	pthread_mutex_lock(&port->lock);
	q = port->queue;
	pthread_mutex_unlock(&port->lock);

	if (q != NULL) {
		struct callout_bundle *b = calloc(1, sizeof(*b));

		if (b == NULL) {
			IOObjectRelease(it);
			return;
		}
		b->callback = w->callback;
		b->refcon = w->refcon;
		b->iterator = it;
		dispatch_async_f(q, b, callout_trampoline);
	} else {
		/* No queue — run on the receive thread directly. Acceptable
		 * for simple consumers (the iter-4 iokitnotifytest is one);
		 * real consumers usually SetDispatchQueue. */
		w->callback(w->refcon, it);
		IOObjectRelease(it);
	}
}

/*
 * Parse hwregd's event text into (id, name, class). Format from
 * notify_watchers in src/hwregd/hwregd.c:
 *     "arrived id=N name=Y class=Z"
 *     "departed id=N name=Y class=Z"
 * Returns true iff `id` was found; name/class default to "".
 */
static bool
parse_event_text(const char *text, uint64_t *id, char *name, size_t name_sz,
    char *klass, size_t klass_sz)
{
	const char *p;

	*id = 0;
	if (name != NULL && name_sz > 0)
		name[0] = '\0';
	if (klass != NULL && klass_sz > 0)
		klass[0] = '\0';

	p = strstr(text, "id=");
	if (p == NULL)
		return (false);
	*id = (uint64_t)strtoull(p + 3, NULL, 10);

	if (name != NULL && (p = strstr(text, "name=")) != NULL) {
		size_t i;

		p += 5;
		for (i = 0; i + 1 < name_sz && p[i] != '\0' && p[i] != ' ';
		    i++)
			name[i] = p[i];
		name[i] = '\0';
	}
	if (klass != NULL && (p = strstr(text, "class=")) != NULL) {
		size_t i;

		p += 6;
		for (i = 0; i + 1 < klass_sz && p[i] != '\0' && p[i] != ' ';
		    i++)
			klass[i] = p[i];
		klass[i] = '\0';
	}
	return (true);
}

/*
 * Does this watch's criteria match the device described by the
 * event text? Empty criteria fields are wildcards (Apple's
 * dictionary-omit semantics).
 */
static bool
event_matches_watch(struct __io_watch *w, uint32_t evt_bit,
    const char *name, const char *klass)
{
	if ((w->event_mask & evt_bit) == 0)
		return (false);
	if (w->criteria.name[0] != '\0' && strcmp(w->criteria.name, name) != 0)
		return (false);
	if (w->criteria.klass[0] != '\0' &&
	    strcmp(w->criteria.klass, klass) != 0)
		return (false);
	/* `driver` is not in the event text — hwregd already filtered
	 * server-side, so a watch with a driver-only criterion gets the
	 * event with no false-positive risk. */
	return (true);
}

static void *
receive_thread(void *arg)
{
	struct IONotificationPort *port = arg;

	while (atomic_load(&port->stop) == 0) {
		struct {
			struct hwreg_event_msg msg;
			mach_msg_max_trailer_t trailer;
		} buf;
		mach_msg_return_t mr;
		uint64_t id;
		uint32_t evt;
		char name[32], klass[32];
		struct __io_watch *w, *snap[16];
		int n_snap = 0, i;

		mr = mach_msg(&buf.msg.hdr,
		    MACH_RCV_MSG | MACH_RCV_TIMEOUT,
		    0, sizeof(buf), port->recv_port,
		    500, MACH_PORT_NULL);
		if (mr == MACH_RCV_TIMED_OUT)
			continue;
		if (mr != MACH_MSG_SUCCESS)
			continue;	/* transient — keep looping */
		if (buf.msg.hdr.msgh_id != HWREG_MSG_EVENT)
			continue;
		if (!parse_event_text(buf.msg.text, &id, name, sizeof(name),
		    klass, sizeof(klass)))
			continue;

		evt = (buf.msg.kind == '+') ? HWREG_EVT_ARRIVED
		    : HWREG_EVT_DEPARTED;

		/* Snapshot the matching watches under the lock into a
		 * small array, then fire outside the lock so a user
		 * callback can safely add or remove notifications. The
		 * 16-entry cap matches hwregd's HWREGD_MAX_WATCHERS / 2;
		 * one IONotificationPort with more than 16 simultaneous
		 * matching watches on a single event is not a real
		 * shape. */
		pthread_mutex_lock(&port->lock);
		for (w = port->watches; w != NULL && n_snap < 16;
		    w = w->next)
			if (event_matches_watch(w, evt, name, klass))
				snap[n_snap++] = w;
		pthread_mutex_unlock(&port->lock);

		for (i = 0; i < n_snap; i++)
			fire_callback(port, snap[i], id);
	}
	return (NULL);
}

IONotificationPortRef
IONotificationPortCreate(mach_port_t mainPort __unused)
{
	struct IONotificationPort *p;
	mach_port_t mp = MACH_PORT_NULL;

	if (mach_port_allocate(mach_task_self(),
	    MACH_PORT_RIGHT_RECEIVE, &mp) != KERN_SUCCESS)
		return (NULL);

	p = calloc(1, sizeof(*p));
	if (p == NULL) {
		(void)mach_port_mod_refs(mach_task_self(), mp,
		    MACH_PORT_RIGHT_RECEIVE, -1);
		return (NULL);
	}
	p->recv_port = mp;
	atomic_store(&p->stop, 0);
	(void)pthread_mutex_init(&p->lock, NULL);
	if (pthread_create(&p->thread, NULL, receive_thread, p) != 0) {
		(void)pthread_mutex_destroy(&p->lock);
		(void)mach_port_mod_refs(mach_task_self(), mp,
		    MACH_PORT_RIGHT_RECEIVE, -1);
		free(p);
		return (NULL);
	}
	return (p);
}

void
IONotificationPortSetDispatchQueue(IONotificationPortRef port,
    dispatch_queue_t queue)
{
	dispatch_queue_t old;

	if (port == NULL)
		return;
	pthread_mutex_lock(&port->lock);
	old = port->queue;
	port->queue = queue;
	if (queue != NULL)
		dispatch_retain(queue);
	pthread_mutex_unlock(&port->lock);
	if (old != NULL)
		dispatch_release(old);
}

mach_port_t
IONotificationPortGetMachPort(IONotificationPortRef port)
{
	return (port != NULL ? port->recv_port : MACH_PORT_NULL);
}

void
IONotificationPortDestroy(IONotificationPortRef port)
{
	struct __io_watch *w, *next;
	mach_port_t svc;

	if (port == NULL)
		return;

	/* Stop the receive thread before tearing the port down. */
	atomic_store(&port->stop, 1);
	(void)pthread_join(port->thread, NULL);

	/* Unwatch each registered watch on hwregd so the daemon
	 * reclaims its watcher table slot. */
	svc = __io_hwregd_port();
	for (w = port->watches; w != NULL; w = next) {
		next = w->next;
		if (svc != MACH_PORT_NULL)
			(void)hwreg_unwatch(svc, w->watcher_id);
		free(w);
	}

	(void)mach_port_mod_refs(mach_task_self(), port->recv_port,
	    MACH_PORT_RIGHT_RECEIVE, -1);
	if (port->queue != NULL)
		dispatch_release(port->queue);
	(void)pthread_mutex_destroy(&port->lock);
	free(port);
}

kern_return_t
IOServiceAddMatchingNotification(IONotificationPortRef port,
    const io_name_t notification_type, CFDictionaryRef matching,
    IOServiceMatchingCallback callback, void *refcon,
    io_iterator_t *out_iterator)
{
	mach_port_t svc = __io_hwregd_port();
	struct __io_watch *w;
	struct io_criteria c;
	hwreg_blob_t critblob;
	uint32_t crit_sz = 0;
	uint32_t mask;
	uint64_t watcher_id = 0;
	kern_return_t kr;

	if (port == NULL || notification_type == NULL || matching == NULL ||
	    callback == NULL || out_iterator == NULL) {
		if (matching != NULL)
			CFRelease(matching);
		return (KERN_INVALID_ARGUMENT);
	}
	if (svc == MACH_PORT_NULL) {
		CFRelease(matching);
		return (kIOReturnNoDevice);
	}

	/* Notification type → hwregd event mask. The three "device
	 * arrived" flavours map to HWREG_EVT_ARRIVED (hwregd has no
	 * internal Publish/FirstMatch/Matched distinction). */
	if (strcmp(notification_type, kIOTerminatedNotification) == 0)
		mask = HWREG_EVT_DEPARTED;
	else
		mask = HWREG_EVT_ARRIVED;

	__io_extract_criteria(matching, &c);
	kr = __io_pack_criteria(&c, critblob, &crit_sz);
	if (kr != KERN_SUCCESS) {
		CFRelease(matching);
		return (kr);
	}

	kr = hwreg_watch(svc, critblob, (mach_msg_type_number_t)crit_sz,
	    mask, port->recv_port, &watcher_id);
	if (kr != KERN_SUCCESS || watcher_id == 0) {
		CFRelease(matching);
		return (kr != KERN_SUCCESS ? kr : kIOReturnError);
	}

	w = calloc(1, sizeof(*w));
	if (w == NULL) {
		(void)hwreg_unwatch(svc, watcher_id);
		CFRelease(matching);
		return (KERN_RESOURCE_SHORTAGE);
	}
	w->watcher_id = watcher_id;
	w->event_mask = mask;
	w->criteria = c;
	w->callback = callback;
	w->refcon = refcon;

	pthread_mutex_lock(&port->lock);
	w->next = port->watches;
	port->watches = w;
	pthread_mutex_unlock(&port->lock);

	/* Initial arming: hand the caller an iterator over the
	 * currently-matching services. IOServiceGetMatchingServices
	 * consumes the matching ref (the contract this routine
	 * also owes the caller). */
	return (IOServiceGetMatchingServices(kIOMainPortDefault,
	    matching, out_iterator));
}
