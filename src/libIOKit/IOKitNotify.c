/*
 * IOKitNotify.c — libIOKit: K2 device-arrival / departure notifications.
 *
 * Each IONotificationPort owns one Mach receive right and one private pthread
 * that drives a raw mach_msg(MACH_RCV_MSG|MACH_RCV_TIMEOUT, 500ms) loop on it.
 * Events fan in on the one port and the receive thread re-matches each event's
 * device fields against the per-watch criteria stored client-side, then fires
 * each matching IOServiceMatchingCallback (via dispatch_async_f if a queue is
 * set, inline on the receive thread otherwise).
 *
 * Backend (#218): watch registration uses the kernel notify channel —
 * ioctl(/dev/ioregistry, IOREGIOCWATCH) registers the recv port directly with
 * the in-kernel registry (#225), and the kernel pushes a binary ioreg_event_msg
 * (msgid IOKIT_NOTIFY_EVENT_MSGID) from its device_attach/device_detach
 * eventhandlers. The kernel is the registry; hwregd has been retired, so there
 * is no userland fallback.
 *
 * Why raw mach_msg and not a libdispatch MACH_RECV source: task #41 + the
 * libSystemConfiguration iter-2 notes confirm DISPATCH_SOURCE_TYPE_MACH_RECV
 * does not reliably deliver in this repo; libSystemConfiguration's SCNotify and
 * this facade both use the same pthread+timed-mach_msg pattern.
 */
#include <IOKit/IOKitLib.h>
#include "IOKitInternal.h"

#include "ioregistry.h"		/* vendored: IOREGIOCWATCH, ioreg_watch_reg, IOREG_EVENT_* */
#include "iokit_notify.h"	/* vendored: ioreg_event_msg + msgid */

#include <dispatch/dispatch.h>
#include <mach/mach.h>

#include <sys/ioctl.h>		/* ioctl(2) on /dev/ioregistry */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * One watch carried as an IOServiceAddMatchingNotification. The criteria
 * fields are kept client-side so that when multiple watches share one
 * IONotificationPort, the receive thread can re-match each event against them
 * and fire the right callback(s). There is no per-watch id — the kernel retires
 * the watch when the recv right is dropped.
 *
 * `event_mask` uses the IOREG_EVENT_* bits (value-identical to the
 * IOKIT_NOTIFY_KIND_* arrive/depart ones, 0x1 / 0x2), so the same mask drives
 * registration and the re-match in event_matches_watch.
 */
struct __io_watch {
	struct __io_watch		*next;
	uint32_t			 event_mask;	/* IOREG_EVENT_* */
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
 * Does this watch's criteria match the device described by the
 * event? Empty criteria fields are wildcards (Apple's
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
	/* `driver` is not in the event payload; a watch with a driver-only
	 * criterion still gets the event (treated as a wildcard here). */
	return (true);
}

/*
 * One mach_msg receive buffer big enough for the kernel notify channel's binary
 * event (ioreg_event_msg, msgid IOKIT_NOTIFY_EVENT_MSGID) plus the largest
 * trailer the kernel might append.
 */
union event_rcv_buf {
	mach_msg_header_t	hdr;
	struct {
		ioreg_event_msg		msg;
		mach_msg_max_trailer_t	trailer;
	} k;
};

/*
 * Decode one received message into (evt, id, name, class). Returns true if the
 * message was a recognized kernel notify event and was decoded; false for an
 * unknown msgid or a malformed body (caller drops it). `evt` is the
 * IOREG_EVENT_* bit (arrive/depart) used by event_matches_watch.
 */
static bool
decode_event(const union event_rcv_buf *buf, uint32_t *evt, uint64_t *id,
    char *name, size_t name_sz, char *klass, size_t klass_sz)
{
	switch (buf->hdr.msgh_id) {
	case IOKIT_NOTIFY_EVENT_MSGID: {
		const ioreg_event_msg *m = &buf->k.msg;
		char tmp_name[IOKIT_NOTIFY_NAME_MAX];
		char tmp_klass[IOKIT_NOTIFY_NAME_MAX];

		/* The kernel re-bounds these, but treat them as untrusted wire
		 * data: copy into a local, force-terminate, then hand out. */
		memcpy(tmp_name, m->name, sizeof(tmp_name));
		memcpy(tmp_klass, m->classname, sizeof(tmp_klass));
		tmp_name[sizeof(tmp_name) - 1] = '\0';
		tmp_klass[sizeof(tmp_klass) - 1] = '\0';

		*id = m->id;
		if (name != NULL && name_sz > 0) {
			strncpy(name, tmp_name, name_sz - 1);
			name[name_sz - 1] = '\0';
		}
		if (klass != NULL && klass_sz > 0) {
			strncpy(klass, tmp_klass, klass_sz - 1);
			klass[klass_sz - 1] = '\0';
		}
		/* DEPART maps to departed; ARRIVE and MATCHED both map to
		 * arrived (the client-side mask only carries arrive/depart). */
		*evt = (m->kind == IOKIT_NOTIFY_KIND_DEPART) ? IOREG_EVENT_DEPART
		    : IOREG_EVENT_ARRIVE;
		return (true);
	}
	default:
		return (false);
	}
}

static void *
receive_thread(void *arg)
{
	struct IONotificationPort *port = arg;

	while (atomic_load(&port->stop) == 0) {
		union event_rcv_buf buf;
		mach_msg_return_t mr;
		uint64_t id;
		uint32_t evt;
		char name[32], klass[32];
		struct __io_watch *w, *snap[16];
		int n_snap = 0, i;

		mr = mach_msg(&buf.hdr,
		    MACH_RCV_MSG | MACH_RCV_TIMEOUT,
		    0, sizeof(buf), port->recv_port,
		    500, MACH_PORT_NULL);
		if (mr == MACH_RCV_TIMED_OUT)
			continue;
		if (mr != MACH_MSG_SUCCESS)
			continue;	/* transient — keep looping */
		if (!decode_event(&buf, &evt, &id, name, sizeof(name),
		    klass, sizeof(klass)))
			continue;

		/* Snapshot the matching watches under the lock into a
		 * small array, then fire outside the lock so a user
		 * callback can safely add or remove notifications. The
		 * 16-entry cap is plenty: one IONotificationPort with more
		 * than 16 simultaneous
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

	/*
	 * Insert a send right into our own name space for `mp` (MAKE_SEND).
	 * The kernel notify channel resolves this port by *name* and needs that
	 * name to carry a send right: IOREGIOCWATCH passes the name to
	 * iokit_notify_copyin_port, which does ipc_object_copyin(...,
	 * MACH_MSG_TYPE_COPY_SEND, ...) on the calling task's IPC space. COPY_SEND
	 * requires the name to already hold a send right (ipc_right_copyin rejects
	 * a name with no MACH_PORT_TYPE_SEND_RIGHTS as KERN_INVALID_RIGHT); a bare
	 * receive-right name fails, the watch never registers, and no event is ever
	 * delivered. (allocate RECEIVE, then insert MAKE_SEND.)
	 */
	if (mach_port_insert_right(mach_task_self(), mp, mp,
	    MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS) {
		(void)mach_port_mod_refs(mach_task_self(), mp,
		    MACH_PORT_RIGHT_RECEIVE, -1);
		return (NULL);
	}

	p = calloc(1, sizeof(*p));
	if (p == NULL) {
		(void)mach_port_mod_refs(mach_task_self(), mp,
		    MACH_PORT_RIGHT_SEND, -1);
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
		    MACH_PORT_RIGHT_SEND, -1);
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

	if (port == NULL)
		return;

	/* Stop the receive thread before tearing the port down. */
	atomic_store(&port->stop, 1);
	(void)pthread_join(port->thread, NULL);

	/* Kernel-channel watches need no explicit teardown — the kernel prunes
	 * the watch when the recv right is dropped below. */
	for (w = port->watches; w != NULL; w = next) {
		next = w->next;
		free(w);
	}

	/* Dropping the recv right signals the kernel notify channel to prune this
	 * port's watches on its next emission (iokit_notify_port_dead). The local
	 * send right inserted at create time (MAKE_SEND) is released too so the
	 * name is fully reclaimed; the kernel holds its own copied send ref
	 * independent of ours, so this does not race its pending sends. */
	(void)mach_port_mod_refs(mach_task_self(), port->recv_port,
	    MACH_PORT_RIGHT_SEND, -1);
	(void)mach_port_mod_refs(mach_task_self(), port->recv_port,
	    MACH_PORT_RIGHT_RECEIVE, -1);
	if (port->queue != NULL)
		dispatch_release(port->queue);
	(void)pthread_mutex_destroy(&port->lock);
	free(port);
}

/*
 * Register the watch on the kernel notify channel: ioctl(/dev/ioregistry,
 * IOREGIOCWATCH) with this port's recv-right name, the flat by-value criteria
 * struct (#218; same fixed struct IOREGIOCLOOKUP uses) and the IOREG_EVENT_*
 * mask. The kernel copies a send right to the port and emits ioreg_event_msg on
 * each matching event. Returns KERN_SUCCESS on success, a kIOReturn* error
 * otherwise. No nvlist packing is involved, so the libxpc-vs-libnv mismatch
 * that broke the #218 round-trip cannot recur.
 */
static kern_return_t
kernel_watch_register(struct IONotificationPort *port,
    const struct io_criteria *c, uint32_t mask)
{
	struct ioreg_watch_reg reg;
	int fd = __io_ioregistry_fd();

	if (fd < 0)
		return (kIOReturnNoDevice);

	memset(&reg, 0, sizeof(reg));
	__io_fill_criteria(c, &reg.criteria);
	reg.event_mask = mask;	/* OR of IOREG_EVENT_* */
	reg.notify_port = (uint32_t)port->recv_port;	/* recv right name */

	if (ioctl(fd, IOREGIOCWATCH, &reg) != 0)
		return (kIOReturnError);
	return (KERN_SUCCESS);
}

kern_return_t
IOServiceAddMatchingNotification(IONotificationPortRef port,
    const io_name_t notification_type, CFDictionaryRef matching,
    IOServiceMatchingCallback callback, void *refcon,
    io_iterator_t *out_iterator)
{
	struct __io_watch *w;
	struct io_criteria c;
	uint32_t mask;
	kern_return_t kr;

	if (port == NULL || notification_type == NULL || matching == NULL ||
	    callback == NULL || out_iterator == NULL) {
		if (matching != NULL)
			CFRelease(matching);
		return (KERN_INVALID_ARGUMENT);
	}

	/* Notification type → event mask. The three "device arrived" flavours
	 * map to IOREG_EVENT_ARRIVE (no internal Publish/FirstMatch/Matched
	 * distinction in the kernel channel). */
	if (strcmp(notification_type, kIOTerminatedNotification) == 0)
		mask = IOREG_EVENT_DEPART;
	else
		mask = IOREG_EVENT_ARRIVE;

	__io_extract_criteria(matching, &c);

	/* Kernel notify channel: the criteria travel as a flat by-value struct
	 * (#218), so there is no nvlist packing and nothing to mismatch —
	 * kernel_watch_register fills it from `c` directly. The kernel has no
	 * per-watch handle and retires the watch when the recv right is dropped. */
	kr = kernel_watch_register(port, &c, mask);
	if (kr != KERN_SUCCESS) {
		CFRelease(matching);
		return (kr);
	}

	w = calloc(1, sizeof(*w));
	if (w == NULL) {
		CFRelease(matching);
		return (KERN_RESOURCE_SHORTAGE);
	}
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
