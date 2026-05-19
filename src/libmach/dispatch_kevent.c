/*
 * dispatch_kevent.c — libmach kevent_qos() + EVFILT_MACHPORT bridge.
 *
 * See <mach/dispatch_kevent.h> for the architecture rationale. Short
 * version: libmach provides a kevent_qos()-style entry point + a
 * widened kevent struct (kevent_qos_s with ext[4]) so libdispatch's
 * HAVE_MACH paths compile against Apple-shape APIs. The wrapper
 * translates kevent_qos_s ↔ FreeBSD's struct kevent at the
 * __sys_kevent boundary, intercepting EVFILT_MACHPORT registrations
 * and routing them through mach.ko's trap-mux op 4 bell mechanism.
 *
 * Concurrency. A single global mutex protects the registration list.
 * The list is expected to stay small (a handful of dispatch_mach_t
 * channels + per-RPC reply ports), so linear search is fine.
 *
 * Limits in this first cut:
 *   - data_out / data_available (kevent_qos's data-buffer feature
 *     used by workloops) are accepted and ignored. No HAVE_MACH-
 *     non-workloop path in libdispatch exercises them.
 *   - ke->qos and ke->ext[2] (priority) are zeroed on synthesized
 *     events. Mach msg priority would come from msgh_voucher/trailer
 *     parsing — deferred to a follow-up.
 *   - kevent_qos's `flags` arg: IMMEDIATE forces zero timeout
 *     downward; WORKQ is ignored; ERROR_EVENTS isn't honored
 *     (errors surface via errno on the function return). libdispatch
 *     only passes these on submit-only calls where the difference
 *     doesn't matter.
 *
 * Buffer ownership. mach_msg() is called with MACH_RCV_LARGE; on
 * MACH_RCV_TOO_LARGE we re-malloc to the reported size and retry.
 * The synthesized event always carries DISPATCH_EV_MSG_NEEDS_FREE
 * (0x10000) in flags so _dispatch_kevent_mach_msg_drain() free()s
 * the buffer after consuming it.
 */

#include <sys/types.h>
#include <sys/event.h>
#include <sys/sysctl.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mach/dispatch_kevent.h>
#include <mach/mach_port.h>
#include <mach/mach_traps.h>
#include <mach/mach_traps_mux.h>
#include <mach/message.h>
#include <mach/port.h>

/*
 * DISPATCH_EV_MSG_NEEDS_FREE — defined inside libdispatch
 * (event_config.h:205). Replicated here so the synth eventlist we
 * hand back tells libdispatch to free() the malloc'd buffer.
 */
#define DISPATCH_EV_MSG_NEEDS_FREE 0x10000

/*
 * MACH_RCV_LARGE / MACH_RCV_TIMEOUT / MACH_RCV_MSG — Apple-canonical
 * mach_msg option bits. Re-declared so this TU doesn't have to pull
 * in libdispatch's internal event_config.h.
 */
#ifndef MACH_RCV_TIMEOUT
#define MACH_RCV_TIMEOUT 0x00000100
#endif
#ifndef MACH_RCV_LARGE
#define MACH_RCV_LARGE 0x00000004
#endif
#ifndef MACH_RCV_MSG
#define MACH_RCV_MSG 0x00000002
#endif
#ifndef MACH_PORT_RIGHT_PORT_SET
#define MACH_PORT_RIGHT_PORT_SET 3
#endif

#define MACH_RCV_INITIAL_BUFSIZE 8192

extern int __sys_kevent(int kq, const struct kevent *changelist, int nchanges,
    struct kevent *eventlist, int nevents, const struct timespec *timeout);

/*
 * Registration record. One per (kq, original ident) tuple.
 *   - wrap_pset: a libmach-owned pset always allocated; for port
 *     idents we move the ident into it; for pset idents we use the
 *     ident itself (mach_port_move_member rejects pset→pset).
 *   - pipe_r/pipe_w: a self-pipe; read end registered on the user's
 *     kq as EVFILT_READ, write end stored in mach.ko via op 4.
 */
struct mach_kev_reg {
	struct mach_kev_reg	*next;
	int			 kq;		/* user's kq fd */
	uint64_t		 ident;		/* original MACHPORT ident */
	mach_port_name_t	 wrap_pset;	/* libmach-owned pset */
	int			 pipe_r;
	int			 pipe_w;
	uint64_t		 udata;		/* original kev->udata */
	uint16_t		 flags;		/* original kev->flags */
	uint32_t		 fflags;	/* original kev->fflags */
	uint32_t		 qos;		/* original kev->qos */
	uint32_t		 xflags;	/* original kev->xflags */
	int64_t			 data;		/* original kev->data */
};

static struct mach_kev_reg	*g_reg_head;
static pthread_mutex_t		 g_reg_mtx = PTHREAD_MUTEX_INITIALIZER;

static struct mach_kev_reg *
reg_find_locked(int kq, uint64_t ident)
{
	struct mach_kev_reg *r;
	for (r = g_reg_head; r != NULL; r = r->next) {
		if (r->kq == kq && r->ident == ident)
			return (r);
	}
	return (NULL);
}

static struct mach_kev_reg *
reg_find_by_pipe_r_locked(int pipe_r)
{
	struct mach_kev_reg *r;
	for (r = g_reg_head; r != NULL; r = r->next) {
		if (r->pipe_r == pipe_r)
			return (r);
	}
	return (NULL);
}

static void
reg_insert_locked(struct mach_kev_reg *r)
{
	r->next = g_reg_head;
	g_reg_head = r;
}

static void
reg_remove_locked(struct mach_kev_reg *target)
{
	struct mach_kev_reg **pp;
	for (pp = &g_reg_head; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == target) {
			*pp = target->next;
			return;
		}
	}
}

int
mach_event_bell_register(mach_port_name_t pset_name, int pipe_w)
{
	static int mux_syscall = -1;
	long ret;

	if (mux_syscall == -1) {
		int tmp;
		size_t len = sizeof(tmp);
		if (sysctlbyname("mach.syscall.mach_trap_mux", &tmp, &len,
		    NULL, 0) != 0)
			return (ENOSYS);
		mux_syscall = tmp;
	}
	ret = syscall(mux_syscall, MACH_TRAP_OP_REGISTER_EVENT_BELL,
	    (uint64_t)pset_name, (uint64_t)(unsigned)pipe_w,
	    (uint64_t)0, (uint64_t)0, (uint64_t)0);
	if (ret != 0)
		return ((int)ret);
	return (0);
}

/*
 * Build the userland half of a registration: allocate a wrap pset,
 * move the original ident into it (if ident is a port; pset-into-
 * pset is rejected by kernel and we fall back to using ident as the
 * bell key directly), create the pipe, arm the bell. Returns 0 on
 * success, errno on failure with all partial state cleaned up.
 */
static int
reg_create(int kq, const struct kevent_qos_s *src, struct mach_kev_reg **out)
{
	struct mach_kev_reg *r;
	mach_port_name_t wrap = MACH_PORT_NULL;
	kern_return_t kr;
	int p[2] = { -1, -1 };
	int err;

	r = calloc(1, sizeof(*r));
	if (r == NULL)
		return (ENOMEM);

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET,
	    &wrap);
	if (kr != KERN_SUCCESS) {
		err = EINVAL;
		goto fail;
	}

	kr = mach_port_move_member(mach_task_self(),
	    (mach_port_name_t)src->ident, wrap);
	if (kr != KERN_SUCCESS) {
		/* ident is likely already a pset. Drop our wrap pset
		 * and use the ident directly as the bell key. */
		(void)mach_port_deallocate(mach_task_self(), wrap);
		wrap = (mach_port_name_t)src->ident;
	}

	if (pipe2(p, O_CLOEXEC | O_NONBLOCK) != 0) {
		err = errno;
		goto fail;
	}

	err = mach_event_bell_register(wrap, p[1]);
	if (err != 0)
		goto fail;

	r->kq = kq;
	r->ident = src->ident;
	r->wrap_pset = wrap;
	r->pipe_r = p[0];
	r->pipe_w = p[1];
	r->udata = src->udata;
	r->flags = src->flags;
	r->fflags = src->fflags;
	r->qos = src->qos;
	r->xflags = src->xflags;
	r->data = src->data;
	*out = r;
	return (0);

fail:
	if (p[0] != -1)
		(void)close(p[0]);
	if (p[1] != -1)
		(void)close(p[1]);
	if (wrap != MACH_PORT_NULL &&
	    wrap != (mach_port_name_t)src->ident) {
		(void)mach_port_deallocate(mach_task_self(), wrap);
	}
	free(r);
	return (err);
}

static void
reg_destroy(struct mach_kev_reg *r)
{
	if (r == NULL)
		return;
	if (r->pipe_r != -1)
		(void)close(r->pipe_r);
	if (r->pipe_w != -1)
		(void)close(r->pipe_w);
	if (r->wrap_pset != MACH_PORT_NULL &&
	    r->wrap_pset != (mach_port_name_t)r->ident) {
		(void)mach_port_deallocate(mach_task_self(), r->wrap_pset);
	}
	free(r);
}

/*
 * Translate one user-supplied MACHPORT changelist entry into the
 * EVFILT_READ entry we submit to __sys_kevent. EV_ADD allocates a
 * registration (or updates an existing one); EV_DELETE looks one up
 * and tears down.
 */
static int
machport_change_translate(int kq, const struct kevent_qos_s *src,
    struct kevent *out)
{
	struct mach_kev_reg *r;
	int err;

	pthread_mutex_lock(&g_reg_mtx);
	if (src->flags & EV_DELETE) {
		r = reg_find_locked(kq, src->ident);
		if (r != NULL)
			reg_remove_locked(r);
		pthread_mutex_unlock(&g_reg_mtx);
		if (r != NULL) {
			EV_SET(out, (uintptr_t)r->pipe_r, EVFILT_READ,
			    EV_DELETE, 0, 0, NULL);
			reg_destroy(r);
		} else {
			/* No matching registration — surface ENOENT
			 * via the real syscall on a sentinel fd. */
			EV_SET(out, (uintptr_t)-1, EVFILT_READ, EV_DELETE,
			    0, 0, NULL);
		}
		return (0);
	}

	r = reg_find_locked(kq, src->ident);
	if (r != NULL) {
		r->udata = src->udata;
		r->flags = src->flags;
		r->fflags = src->fflags;
		r->qos = src->qos;
		r->xflags = src->xflags;
		r->data = src->data;
		EV_SET(out, (uintptr_t)r->pipe_r, EVFILT_READ,
		    EV_ADD | EV_CLEAR, 0, 0, r);
		pthread_mutex_unlock(&g_reg_mtx);
		return (0);
	}
	pthread_mutex_unlock(&g_reg_mtx);

	err = reg_create(kq, src, &r);
	if (err != 0) {
		errno = err;
		return (-1);
	}
	pthread_mutex_lock(&g_reg_mtx);
	reg_insert_locked(r);
	pthread_mutex_unlock(&g_reg_mtx);
	EV_SET(out, (uintptr_t)r->pipe_r, EVFILT_READ, EV_ADD | EV_CLEAR,
	    0, 0, r);
	return (0);
}

/*
 * Drain the wakeup byte and mach_msg-receive on the pset, filling
 * the synthesized event's ext[0..1] and fflags.
 */
static void
machport_recv_into(struct mach_kev_reg *r, struct kevent_qos_s *synth)
{
	char drain_buf[16];
	mach_msg_header_t *hdr;
	mach_msg_return_t kr;
	mach_msg_size_t bufsize = MACH_RCV_INITIAL_BUFSIZE;

	(void)read(r->pipe_r, drain_buf, sizeof(drain_buf));

	hdr = malloc(bufsize);
	if (hdr == NULL) {
		memset(synth, 0, sizeof(*synth));
		synth->ident = r->ident;
		synth->filter = EVFILT_MACHPORT;
		synth->flags = r->flags;
		synth->fflags = (uint32_t)MACH_RCV_BODY_ERROR;
		synth->udata = r->udata;
		return;
	}
	kr = mach_msg(hdr, MACH_RCV_MSG | MACH_RCV_LARGE | MACH_RCV_TIMEOUT,
	    0, bufsize, r->wrap_pset, 0, MACH_PORT_NULL);

	if (kr == MACH_RCV_TOO_LARGE) {
		mach_msg_size_t need = hdr->msgh_size;
		free(hdr);
		hdr = malloc(need);
		if (hdr != NULL) {
			bufsize = need;
			kr = mach_msg(hdr,
			    MACH_RCV_MSG | MACH_RCV_LARGE | MACH_RCV_TIMEOUT,
			    0, bufsize, r->wrap_pset, 0, MACH_PORT_NULL);
		}
	}

	memset(synth, 0, sizeof(*synth));
	synth->ident = r->ident;
	synth->filter = EVFILT_MACHPORT;
	synth->flags = r->flags | DISPATCH_EV_MSG_NEEDS_FREE;
	synth->fflags = (uint32_t)kr;
	synth->data = r->data;
	synth->udata = r->udata;
	synth->qos = r->qos;
	synth->xflags = r->xflags;
	if (kr == MACH_MSG_SUCCESS && hdr != NULL) {
		synth->ext[0] = (uint64_t)(uintptr_t)hdr;
		synth->ext[1] = bufsize;
	}
}

/*
 * Translate a FreeBSD struct kevent (output of __sys_kevent for a
 * non-mach changelist entry) back to kevent_qos_s. ext[] and qos
 * are zeroed.
 */
static void
kev_to_qos(const struct kevent *in, struct kevent_qos_s *out)
{
	memset(out, 0, sizeof(*out));
	out->ident = (uint64_t)in->ident;
	out->filter = in->filter;
	out->flags = in->flags;
	out->fflags = in->fflags;
	out->data = in->data;
	out->udata = (uint64_t)(uintptr_t)in->udata;
}

/*
 * Translate kevent_qos_s → struct kevent for non-MACHPORT entries.
 * ext[] and qos/xflags are dropped at the FreeBSD boundary.
 */
static void
qos_to_kev(const struct kevent_qos_s *in, struct kevent *out)
{
	EV_SET(out, (uintptr_t)in->ident, in->filter, in->flags,
	    in->fflags, in->data, (void *)(uintptr_t)in->udata);
}

#define KEVENT_STACK_SLOTS 16

int
kevent_qos(int kq, const struct kevent_qos_s *changelist, int nchanges,
    struct kevent_qos_s *eventlist, int nevents,
    void *data_out, size_t *data_available, unsigned int flags)
{
	struct kevent stack_ch[KEVENT_STACK_SLOTS];
	struct kevent stack_ev[KEVENT_STACK_SLOTS];
	struct kevent *scratch_ch = stack_ch;
	struct kevent *scratch_ev = stack_ev;
	struct timespec zero_ts = { 0, 0 };
	const struct timespec *timeout = NULL;
	int saved_errno = 0;
	int i, n, rc;

	(void)data_out;
	if (data_available != NULL)
		*data_available = 0;

	if (flags & KEVENT_FLAG_IMMEDIATE)
		timeout = &zero_ts;

	if (nchanges > 0) {
		if (nchanges > KEVENT_STACK_SLOTS) {
			scratch_ch = calloc((size_t)nchanges,
			    sizeof(*scratch_ch));
			if (scratch_ch == NULL) {
				errno = ENOMEM;
				return (-1);
			}
		}
		for (i = 0; i < nchanges; i++) {
			if (changelist[i].filter == EVFILT_MACHPORT) {
				if (machport_change_translate(kq,
				    &changelist[i], &scratch_ch[i]) != 0) {
					saved_errno = errno;
					EV_SET(&scratch_ch[i], (uintptr_t)-1,
					    EVFILT_READ, EV_ADD, 0, 0, NULL);
				}
			} else {
				qos_to_kev(&changelist[i], &scratch_ch[i]);
			}
		}
	}

	if (nevents > KEVENT_STACK_SLOTS) {
		scratch_ev = calloc((size_t)nevents, sizeof(*scratch_ev));
		if (scratch_ev == NULL) {
			if (nchanges > KEVENT_STACK_SLOTS)
				free(scratch_ch);
			errno = ENOMEM;
			return (-1);
		}
	}

	rc = __sys_kevent(kq, nchanges > 0 ? scratch_ch : NULL, nchanges,
	    nevents > 0 ? scratch_ev : NULL, nevents, timeout);

	if (nchanges > KEVENT_STACK_SLOTS)
		free(scratch_ch);

	if (rc < 0) {
		if (nevents > KEVENT_STACK_SLOTS)
			free(scratch_ev);
		if (saved_errno != 0)
			errno = saved_errno;
		return (rc);
	}

	n = rc;
	for (i = 0; i < n; i++) {
		struct mach_kev_reg *r = NULL;

		if (scratch_ev[i].filter == EVFILT_READ) {
			pthread_mutex_lock(&g_reg_mtx);
			r = reg_find_by_pipe_r_locked(
			    (int)scratch_ev[i].ident);
			pthread_mutex_unlock(&g_reg_mtx);
		}
		if (r != NULL) {
			machport_recv_into(r, &eventlist[i]);
		} else {
			kev_to_qos(&scratch_ev[i], &eventlist[i]);
		}
	}

	if (nevents > KEVENT_STACK_SLOTS)
		free(scratch_ev);

	if (saved_errno != 0)
		errno = saved_errno;
	return (rc);
}
