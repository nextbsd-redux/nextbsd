/*
 * mach/dispatch_kevent.h — Apple-shape kevent_qos surface on FreeBSD.
 *
 * Background. Apple's libdispatch (mach.c + event/event_kevent.c)
 * uses two XNU-private kqueue extensions that FreeBSD doesn't have:
 *   - EVFILT_MACHPORT, a filter that delivers Mach messages inline
 *     with the kevent (kernel does the mach_msg server-side; ke->ext[0]
 *     points at the buffer, ext[1] holds size, fflags carries kr);
 *   - kevent_qos(), a wider entry point taking struct kevent_qos_s
 *     (ext[4], qos, xflags) instead of struct kevent.
 *
 * Approach (task #39 Path B). Rather than patch ~hundreds of ext[]
 * accesses across libdispatch's mach paths, we provide both
 * extensions in libmach:
 *
 *   1. struct kevent_qos_s — same layout as Apple's, with ext[4].
 *      libdispatch typedefs dispatch_kevent_s to this via
 *      DISPATCH_USE_KEVENT_QOS (= 1 when EV_SET_QOS is defined,
 *      which this header does).
 *
 *   2. kevent_qos() — wrapper that translates kevent_qos_s ↔
 *      struct kevent at the FreeBSD __sys_kevent boundary. Non-mach
 *      changelist entries pass through; EVFILT_MACHPORT entries get
 *      routed through mach.ko's trap-mux op 4 bell mechanism (see
 *      mach_kmod/src/mach_event_bridge.c). Returned EVFILT_READ
 *      events on libmach-owned pipes are transmuted back into
 *      Apple-shape EVFILT_MACHPORT events, complete with a mach_msg
 *      recv'd buffer in ext[0] and size in ext[1].
 *
 *   3. EVFILT_MACHPORT — defined to a libmach-private sentinel
 *      value (-22) that never reaches the kernel; the wrapper
 *      intercepts before __sys_kevent and translates after.
 *
 *   4. DISPATCH_USE_KEVENT_WORKLOOP — stays 0 on FreeBSD because
 *      HAVE_PTHREAD_WORKQUEUE_WORKLOOP is not defined (internal.h
 *      :751). We do NOT provide EVFILT_WORKLOOP, kqueue_workloop_*,
 *      or the workloop bind/leave primitives.
 *
 * Coverage. libdispatch's mach.c registers two shapes through
 * EVFILT_MACHPORT:
 *   - Channel receive port (mach_recv) — typically a port set;
 *   - Per-RPC reply ports (mach_reply) — single ports.
 * Both go through the same wrapper. Single ports are transparently
 * wrapped in a libmach-owned single-member pset so the kernel bell
 * (pset-keyed) works for either case.
 */
#ifndef _MACH_DISPATCH_KEVENT_H_
#define _MACH_DISPATCH_KEVENT_H_

#include <stdint.h>
#include <stddef.h>
#include <sys/event.h>		/* struct kevent (FreeBSD-shape), EVFILT_* */
#include <mach/port.h>		/* mach_port_name_t */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * EVFILT_MACHPORT — libmach-private sentinel. Selected to:
 *   - be negative (Apple convention, libdispatch comparisons assume
 *     signed values like Apple's -8);
 *   - sit outside FreeBSD's valid filter range -1..-15 so any
 *     accidental path to __sys_kevent gets EINVAL not collision;
 *   - continue the "-EVFILT_SYSCOUNT - N" pattern used by
 *     libdispatch's own synthetic filters in event_config.h:212.
 */
#ifndef EVFILT_MACHPORT
#define EVFILT_MACHPORT (-22)
#endif

/*
 * kevent_qos_s — matches Apple's <sys/event.h> declaration exactly.
 *
 * Field order is wire-stable on macOS (XNU treats this as ABI).
 * libdispatch zero-initializes via designated initializers (.ident,
 * .filter, …) so field-add at the end is forward-compatible. Don't
 * reorder existing fields.
 */
struct kevent_qos_s {
	uint64_t	ident;		/* identifier for this event */
	int16_t		filter;		/* filter for event */
	uint16_t	flags;		/* general flags */
	uint32_t	qos;		/* quality of service */
	uint64_t	udata;		/* opaque user data identifier */
	uint32_t	fflags;		/* filter-specific flags */
	uint32_t	xflags;		/* extra filter-specific flags */
	int64_t		data;		/* filter-specific data */
	uint64_t	ext[4];		/* filter-specific extensions */
};

/*
 * EV_SET_QOS — initialize a kevent_qos_s. Its presence is what
 * gates DISPATCH_USE_KEVENT_QOS=1 in libdispatch's event_config.h.
 * Apple's macro takes a variadic ext list; we keep it explicit.
 */
#ifndef EV_SET_QOS
#define EV_SET_QOS(kevp, _ident, _filter, _flags, _qos, _udata, _fflags, \
    _xflags, _data, _e0, _e1, _e2, _e3) do {				\
	struct kevent_qos_s *_kevp_ = (kevp);				\
	_kevp_->ident   = (uint64_t)(_ident);				\
	_kevp_->filter  = (int16_t)(_filter);				\
	_kevp_->flags   = (uint16_t)(_flags);				\
	_kevp_->qos     = (uint32_t)(_qos);				\
	_kevp_->udata   = (uint64_t)(_udata);				\
	_kevp_->fflags  = (uint32_t)(_fflags);				\
	_kevp_->xflags  = (uint32_t)(_xflags);				\
	_kevp_->data    = (int64_t)(_data);				\
	_kevp_->ext[0]  = (uint64_t)(_e0);				\
	_kevp_->ext[1]  = (uint64_t)(_e1);				\
	_kevp_->ext[2]  = (uint64_t)(_e2);				\
	_kevp_->ext[3]  = (uint64_t)(_e3);				\
} while (0)
#endif

/*
 * KEVENT_FLAG_* — Apple's kevent_qos `flags` arg bits. libdispatch
 * passes KEVENT_FLAG_IMMEDIATE | KEVENT_FLAG_WORKQ on a few paths.
 * We accept and ignore most; IMMEDIATE maps to a zero timespec on
 * the underlying __sys_kevent call.
 */
#ifndef KEVENT_FLAG_NONE
#define KEVENT_FLAG_NONE		0x0000
#endif
#ifndef KEVENT_FLAG_IMMEDIATE
#define KEVENT_FLAG_IMMEDIATE		0x0001
#endif
#ifndef KEVENT_FLAG_ERROR_EVENTS
#define KEVENT_FLAG_ERROR_EVENTS	0x0002
#endif
#ifndef KEVENT_FLAG_STACK_EVENTS
#define KEVENT_FLAG_STACK_EVENTS	0x0004
#endif
#ifndef KEVENT_FLAG_STACK_DATA
#define KEVENT_FLAG_STACK_DATA		0x0008
#endif
#ifndef KEVENT_FLAG_WORKQ
#define KEVENT_FLAG_WORKQ		0x0040
#endif

/*
 * kevent_qos — Apple-signature kevent entry point.
 *
 * data_out / data_available are for kevent_qos's data-buffer feature
 * (used by workloop machinery, which we don't have). We accept them
 * for ABI compat: if non-NULL data_out is passed we ignore it and
 * set *data_available = 0 — no callers in libdispatch's HAVE_MACH
 * non-workloop paths exercise it.
 */
int kevent_qos(int kq, const struct kevent_qos_s *changelist, int nchanges,
    struct kevent_qos_s *eventlist, int nevents,
    void *data_out, size_t *data_available, unsigned int flags);

/*
 * mach_event_bell_register — userland wrapper around trap-mux op 4.
 * Registers `pipe_w` as the wakeup file descriptor for `pset_name`.
 * Returns 0 on success, errno on failure.
 *
 * Exposed in this header so other consumers (tests, future libxpc
 * runloop integration) can register additional psets without going
 * through libdispatch.
 */
int mach_event_bell_register(mach_port_name_t pset_name, int pipe_w);

/*
 * mach_event_bell_unregister — trap-mux op 5. Drop the bell op 4
 * armed so the kernel stops writing wakeup bytes to the pipe.
 * Returns 0 on success, errno on failure.
 */
int mach_event_bell_unregister(mach_port_name_t pset_name);

#ifdef __cplusplus
}
#endif

#endif /* !_MACH_DISPATCH_KEVENT_H_ */
