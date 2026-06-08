/*
 * test_evfilt_machport_concurrent — concurrency stress for the native kernel
 * EVFILT_MACHPORT filter. Companion to test_evfilt_machport (the single-
 * threaded delivery probe, #249). #168 Stage 0 / #251.
 *
 * Purpose: pound the kernel paths that the single-threaded probe never
 * touches — concurrent knote attach/detach, port-set teardown with a live
 * knote, and mach_port_move_member churning a set's membership while another
 * thread is mid-filt_machportattach. These are exactly the races behind the
 * PR #250 boot panics:
 *
 *   - #253: UAF in filt_machportattach — it dropped the pset lock before
 *     taking a reference, so a concurrent move_member could free the pset out
 *     from under the attaching knote. Reproduced here by attacher threads and
 *     mover threads hammering one SHARED port set at once.
 *   - #252: NULL td_machdata deref in filt_machport / filt_machportattach on
 *     threads that never went through mach.ko's per-thread init. Every worker
 *     here is a raw pthread (libdispatch-style worker), so its td_machdata is
 *     NULL on first Mach touch — the lazy-init path is exercised by every
 *     attach.
 *   - #148: page fault in ipc_kmsg_destroy when such a pthread EXITS and the
 *     kernel walks its fd table closing Mach ports. Each worker allocates and
 *     drops Mach rights, then exits, so thread teardown runs the kmsg-destroy
 *     path repeatedly.
 *
 * If any of those regress, the kernel panics and CI's boot test catches it.
 * If they hold, every worker completes and we print EVFILT-MACHPORT-CONCURRENT-OK.
 *
 * NOTE on coverage: CI boots qemu with a single vcpu (and TCG thread=single),
 * so true parallelism is limited — this is a strong path/teardown exerciser
 * there and a genuine race detector on KVM / multi-core hardware. It is
 * written to be safe and terminating either way (bounded iterations, short
 * timeouts, all threads joined).
 *
 * Raw FreeBSD kevent(2) with the native filter number (-16); deliberately does
 * NOT include <mach/dispatch_kevent.h> (which would redefine EVFILT_MACHPORT to
 * the libmach -22 bridge sentinel). See test_evfilt_machport.c for the rationale.
 *
 * Markers (greppable by tests/boot-test.sh):
 *   EVFILT-MACHPORT-CONCURRENT-OK    — all workers finished, no panic
 *   EVFILT-MACHPORT-CONCURRENT-FAIL  — a worker hit an unexpected hard error
 *   EVFILT-MACHPORT-CONCURRENT-SKIP  — native filter unavailable on this kernel
 */
#include <sys/types.h>
#include <sys/event.h>		/* kqueue(2), struct kevent (ext[4] on FreeBSD) */
#include <sys/time.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <mach/mach_traps.h>	/* mach_task_self */
#include <mach/mach_port.h>	/* mach_port_allocate / _insert_right / _move_member / _mod_refs */
#include <mach/message.h>	/* mach_msg, MACH_RCV_MSG, MACH_MSG_TYPE_* */

#define	EVFILT_MACHPORT_NATIVE	(-16)
#define	TEST_MSG_ID		0x45564d50	/* "EVMP" */

#define	N_ATTACH		6	/* threads registering/dropping knotes */
#define	N_MOVE			4	/* threads churning set membership + sending */
#define	N_SHARED_PORTS		8	/* ports moved in/out of the shared set */
#define	ITERS			300	/* per-thread iterations */

#ifndef MACH_PORT_RIGHT_PORT_SET
#define	MACH_PORT_RIGHT_PORT_SET	3
#endif

/* The shared port set every worker contends on (the #253 race surface). */
static mach_port_name_t	g_pset = MACH_PORT_NULL;
/* Ports the mover threads shuffle in and out of g_pset. */
static mach_port_name_t	g_ports[N_SHARED_PORTS];

/* SKIP/FAIL signalling out of the worker threads. */
static atomic_int	g_skip;		/* native filter rejected — nothing to test */
static atomic_int	g_fail;		/* an unexpected hard error */

/*
 * Attacher worker: repeatedly register an EVFILT_MACHPORT knote on the shared
 * set, let it sit briefly (so a mover thread can race membership against the
 * live attach), then tear it down. Every other iteration also spins up a
 * throwaway private port set, attaches a knote, and destroys the set while the
 * knote is still live — exercising ipc_pset_destroy's knote-eviction path
 * (#29) under contention.
 */
static void *
attach_worker(void *arg)
{
	mach_port_name_t task = mach_task_self();
	int id = (int)(intptr_t)arg;
	int i;

	for (i = 0; i < ITERS; i++) {
		struct kevent kev;
		int kq;

		if (atomic_load(&g_skip) || atomic_load(&g_fail))
			return (NULL);

		kq = kqueue();
		if (kq < 0) {
			atomic_store(&g_fail, 1);
			return (NULL);
		}

		(void)memset(&kev, 0, sizeof(kev));
		kev.ident = g_pset;
		kev.filter = EVFILT_MACHPORT_NATIVE;
		kev.flags = EV_ADD | EV_ENABLE;
		kev.fflags = MACH_RCV_MSG;	/* notify mode, no recv buffer */
		if (kevent(kq, &kev, 1, NULL, 0, NULL) < 0) {
			/* Filter -16 unavailable on this kernel — discovery SKIP. */
			atomic_store(&g_skip, 1);
			(void)close(kq);
			return (NULL);
		}

		/* Give a mover thread a window to race membership vs this attach. */
		usleep(50);

		(void)memset(&kev, 0, sizeof(kev));
		kev.ident = g_pset;
		kev.filter = EVFILT_MACHPORT_NATIVE;
		kev.flags = EV_DELETE;
		(void)kevent(kq, &kev, 1, NULL, 0, NULL);

		/*
		 * Half the time, also create + destroy a private set with a live
		 * knote attached (knote still registered when the set is torn
		 * down — the #29 ipc_pset_destroy eviction path).
		 */
		if ((i & 1) == 0) {
			mach_port_name_t ps = MACH_PORT_NULL;

			if (mach_port_allocate(task, MACH_PORT_RIGHT_PORT_SET,
			    &ps) == KERN_SUCCESS) {
				(void)memset(&kev, 0, sizeof(kev));
				kev.ident = ps;
				kev.filter = EVFILT_MACHPORT_NATIVE;
				kev.flags = EV_ADD | EV_ENABLE;
				kev.fflags = MACH_RCV_MSG;
				(void)kevent(kq, &kev, 1, NULL, 0, NULL);
				/* Destroy the set out from under the live knote. */
				(void)mach_port_mod_refs(task, ps,
				    MACH_PORT_RIGHT_PORT_SET, -1);
			}
		}

		(void)close(kq);	/* detaches any remaining knote */
		(void)id;
	}
	return (NULL);
}

/*
 * Mover worker: churn the shared set's membership while attachers are mid-
 * attach, and send messages to ports as they pass through the set so the
 * filter's inline-receive / signal path runs against changing membership.
 */
static void *
move_worker(void *arg)
{
	mach_port_name_t task = mach_task_self();
	int id = (int)(intptr_t)arg;
	int i;

	for (i = 0; i < ITERS; i++) {
		int p;

		if (atomic_load(&g_skip) || atomic_load(&g_fail))
			return (NULL);

		for (p = 0; p < N_SHARED_PORTS; p++) {
			mach_port_name_t port = g_ports[p];
			mach_msg_header_t hdr;

			if (port == MACH_PORT_NULL)
				continue;

			/* Into the shared set (races filt_machportattach). */
			(void)mach_port_move_member(task, port, g_pset);

			/* Enqueue a message so ipc_pset_signal wakes any knote. */
			(void)memset(&hdr, 0, sizeof(hdr));
			hdr.msgh_bits =
			    MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND_ONCE, 0);
			hdr.msgh_size = sizeof(hdr);
			hdr.msgh_remote_port = port;
			hdr.msgh_local_port = MACH_PORT_NULL;
			hdr.msgh_id = TEST_MSG_ID;
			(void)mach_msg(&hdr, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
			    sizeof(hdr), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);

			/* Back out of the set (move to the null set = remove). */
			(void)mach_port_move_member(task, port, MACH_PORT_NULL);
		}
		(void)id;
	}
	return (NULL);
}

int
main(void)
{
	mach_port_name_t task = mach_task_self();
	pthread_t at[N_ATTACH], mt[N_MOVE];
	int i;

	if (task == MACH_PORT_NULL) {
		printf("EVFILT-MACHPORT-CONCURRENT-FAIL: mach_task_self == NULL\n");
		return (1);
	}

	/* The shared contended set. */
	if (mach_port_allocate(task, MACH_PORT_RIGHT_PORT_SET, &g_pset)
	    != KERN_SUCCESS) {
		printf("EVFILT-MACHPORT-CONCURRENT-FAIL: shared pset allocate\n");
		return (1);
	}

	/* The pool of receive ports (with self-send rights) the movers shuffle. */
	for (i = 0; i < N_SHARED_PORTS; i++) {
		mach_port_name_t port = MACH_PORT_NULL;

		if (mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &port)
		    != KERN_SUCCESS) {
			printf("EVFILT-MACHPORT-CONCURRENT-FAIL: port allocate\n");
			return (1);
		}
		(void)mach_port_insert_right(task, port, port,
		    MACH_MSG_TYPE_MAKE_SEND);
		g_ports[i] = port;
	}

	for (i = 0; i < N_ATTACH; i++) {
		if (pthread_create(&at[i], NULL, attach_worker,
		    (void *)(intptr_t)i) != 0) {
			printf("EVFILT-MACHPORT-CONCURRENT-FAIL: pthread_create\n");
			return (1);
		}
	}
	for (i = 0; i < N_MOVE; i++) {
		if (pthread_create(&mt[i], NULL, move_worker,
		    (void *)(intptr_t)i) != 0) {
			printf("EVFILT-MACHPORT-CONCURRENT-FAIL: pthread_create\n");
			return (1);
		}
	}

	for (i = 0; i < N_ATTACH; i++)
		(void)pthread_join(at[i], NULL);
	for (i = 0; i < N_MOVE; i++)
		(void)pthread_join(mt[i], NULL);

	if (atomic_load(&g_skip)) {
		printf("EVFILT-MACHPORT-CONCURRENT-SKIP: native filter unavailable "
		    "on this kernel\n");
		return (0);
	}
	if (atomic_load(&g_fail)) {
		printf("EVFILT-MACHPORT-CONCURRENT-FAIL: a worker hit an "
		    "unexpected error\n");
		return (1);
	}

	printf("EVFILT-MACHPORT-CONCURRENT-OK: %d attach + %d move workers x %d "
	    "iters, no panic under concurrent attach/detach/destroy + "
	    "move_member churn\n", N_ATTACH, N_MOVE, ITERS);
	return (0);
}
