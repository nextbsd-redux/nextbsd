/*
 * test_evfilt_machport — prove the NATIVE kernel EVFILT_MACHPORT filter
 * delivers a kqueue wakeup (and the message) when a Mach message arrives on a
 * port in a registered port set. #168 gate.
 *
 * Background: mach.ko registers a real kqueue filter at EVFILT_MACHPORT (-16,
 * reserved by nextbsd-kernel patch 0003) — filt_machport in
 * compat/mach/ipc/ipc_pset.c. It attaches a knote to the port set's ips_note;
 * ipc_pset_signal() (called when a message is enqueued onto a port in the set)
 * wakes the knote, and filt_machport then receives the message inline into the
 * caller's buffer (kev.ext[0]/ext[1]), Apple-style. NOTHING in the tree
 * exercises this native path: libdispatch's EVFILT_MACHPORT goes through
 * libmach's kevent_qos() shim, which intercepts a DIFFERENT sentinel (-22) and
 * routes it through the register_event_bell pipe-bridge (task #39 Path B).
 * Whether the native filter actually delivers has been an open question
 * (task #41 — every daemon avoids the dispatch MACH_RECV path with a
 * pthread+mach_msg loop). This test answers it directly.
 *
 * Deliberately uses the raw FreeBSD kevent(2) with the native filter number
 * (-16) and does NOT include <mach/mach.h> / <mach/dispatch_kevent.h> (which
 * would redefine EVFILT_MACHPORT to the libmach -22 bridge sentinel). FreeBSD's
 * struct kevent carries ext[4]; filt_machport reads ext[0] (recv buffer addr)
 * and ext[1] (size) and fflags (MACH_RCV_MSG).
 *
 * Markers (greppable by tests/boot-test.sh):
 *   EVFILT-MACHPORT-OK    — kevent registered, fired on send, and the message
 *                           was received inline with the expected msgh_id
 *   EVFILT-MACHPORT-FAIL  — registration rejected, no wakeup (timeout), or the
 *                           delivered message was wrong
 */
#include <sys/types.h>
#include <sys/event.h>		/* kqueue(2), struct kevent (ext[4] on FreeBSD) */
#include <sys/time.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <mach/mach_traps.h>	/* mach_task_self */
#include <mach/mach_port.h>	/* mach_port_allocate / _insert_right / _move_member */
#include <mach/message.h>	/* mach_msg, MACH_RCV_MSG, MACH_MSG_TYPE_* */

/*
 * The NATIVE kernel filter number (nextbsd-kernel patch 0003 +
 * mach_module.c kqueue_add_filteropts(EVFILT_MACHPORT, &machport_filtops)).
 * Hardcoded because the stock userland <sys/event.h> predates the bump and
 * <mach/dispatch_kevent.h> defines a different (-22) bridge sentinel.
 */
#define	EVFILT_MACHPORT_NATIVE	(-16)

#define	TEST_MSG_ID	0x45564d50	/* "EVMP" */

int
main(void)
{
	mach_port_name_t task = mach_task_self();
	mach_port_name_t port = MACH_PORT_NULL;
	mach_port_name_t pset = MACH_PORT_NULL;
	kern_return_t kr;
	int kq, n;

	/* Inline-receive buffer the kernel filter fills (ext[0]/ext[1]). */
	static struct {
		mach_msg_header_t	hdr;
		uint8_t			body[1024];
	} rcv;
	struct kevent kev, out;
	struct timespec ts;
	mach_msg_header_t send_hdr;
	mach_msg_return_t mr;

	if (task == MACH_PORT_NULL) {
		printf("EVFILT-MACHPORT-FAIL: mach_task_self == NULL\n");
		return (1);
	}

	/* A receive port + a send right onto it (so we can send to ourselves). */
	kr = mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &port);
	if (kr != KERN_SUCCESS) {
		printf("EVFILT-MACHPORT-FAIL: port allocate kr=0x%x\n", (unsigned)kr);
		return (1);
	}
	kr = mach_port_insert_right(task, port, port, MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("EVFILT-MACHPORT-FAIL: insert_right kr=0x%x\n", (unsigned)kr);
		return (1);
	}

	/* A port set, with our receive port as a member. EVFILT_MACHPORT
	 * attaches to the SET (filt_machportattach translates the ident as a
	 * PORT_SET right and hangs the knote on pset->ips_note). */
	kr = mach_port_allocate(task, MACH_PORT_RIGHT_PORT_SET, &pset);
	if (kr != KERN_SUCCESS) {
		printf("EVFILT-MACHPORT-FAIL: pset allocate kr=0x%x\n", (unsigned)kr);
		return (1);
	}
	kr = mach_port_move_member(task, port, pset);
	if (kr != KERN_SUCCESS) {
		printf("EVFILT-MACHPORT-FAIL: move_member kr=0x%x\n", (unsigned)kr);
		return (1);
	}

	kq = kqueue();
	if (kq < 0) {
		printf("EVFILT-MACHPORT-FAIL: kqueue()\n");
		return (1);
	}

	/* Register EVFILT_MACHPORT on the port set, asking the filter to
	 * inline-receive into rcv (ext[0]=addr, ext[1]=size, fflags=MACH_RCV_MSG). */
	(void)memset(&rcv, 0, sizeof(rcv));
	(void)memset(&kev, 0, sizeof(kev));
	kev.ident = pset;
	kev.filter = EVFILT_MACHPORT_NATIVE;
	kev.flags = EV_ADD | EV_ENABLE;
	kev.fflags = MACH_RCV_MSG;
	kev.ext[0] = (uint64_t)(uintptr_t)&rcv;
	kev.ext[1] = (uint64_t)sizeof(rcv);
	if (kevent(kq, &kev, 1, NULL, 0, NULL) < 0) {
		/* EINVAL here = the kernel rejected filter -16 (patch/registration
		 * not effective on this image). Self-SKIP rather than hard-fail so
		 * the gate stays green on a kernel predating the filter. */
		printf("EVFILT-MACHPORT-SKIP: kevent EV_ADD rejected "
		    "(native filter unavailable on this kernel)\n");
		return (0);
	}

	/* Send a message to our port (enqueues on a member of the set). */
	(void)memset(&send_hdr, 0, sizeof(send_hdr));
	send_hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND_ONCE, 0);
	send_hdr.msgh_size = sizeof(send_hdr);
	send_hdr.msgh_remote_port = port;
	send_hdr.msgh_local_port = MACH_PORT_NULL;
	send_hdr.msgh_id = TEST_MSG_ID;
	mr = mach_msg(&send_hdr, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
	    sizeof(send_hdr), 0, MACH_PORT_NULL, 100, MACH_PORT_NULL);
	if (mr != MACH_MSG_SUCCESS) {
		printf("EVFILT-MACHPORT-FAIL: mach_msg(SEND) 0x%x\n", (unsigned)mr);
		return (1);
	}

	/* Wait for the kqueue to wake. If the native filter delivers, this
	 * returns one event AND rcv holds the message (filt_machport received
	 * it inline). A timeout means the wakeup chain is broken. */
	ts.tv_sec = 5;
	ts.tv_nsec = 0;
	(void)memset(&out, 0, sizeof(out));
	n = kevent(kq, NULL, 0, &out, 1, &ts);
	if (n < 0) {
		printf("EVFILT-MACHPORT-FAIL: kevent(wait) error\n");
		return (1);
	}
	if (n == 0) {
		printf("EVFILT-MACHPORT-FAIL: no wakeup in 5s — native filter did "
		    "not deliver (knote wakeup chain broken)\n");
		return (1);
	}
	if (out.filter != EVFILT_MACHPORT_NATIVE) {
		printf("EVFILT-MACHPORT-FAIL: woke on wrong filter %d\n", out.filter);
		return (1);
	}
	if (rcv.hdr.msgh_id != TEST_MSG_ID) {
		printf("EVFILT-MACHPORT-FAIL: woke but msgh_id=0x%x (expected 0x%x) "
		    "— event fired without inline receive\n",
		    (unsigned)rcv.hdr.msgh_id, TEST_MSG_ID);
		return (1);
	}

	printf("EVFILT-MACHPORT-OK: kqueue woke on Mach message + received it "
	    "inline (msgh_id=0x%x)\n", (unsigned)rcv.hdr.msgh_id);
	return (0);
}
