/*
 * hwregtest — iter 3b-ii test client for hwregd's Mach pub/sub bus.
 *
 * Looks up the org.freebsd.hwregd Mach service, subscribes a notify
 * port, and confirms it receives the subscription-ack EVENT message.
 * Prints HWREG-PUBSUB-OK on success — the CI boot test (run.sh)
 * matches that marker. The protocol constants and message struct
 * mirror hwregd.c.
 */
#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <stdio.h>
#include <string.h>

#define HWREG_MSG_SUBSCRIBE	0x48575201	/* client -> hwregd */
#define HWREG_MSG_EVENT		0x48575202	/* hwregd -> subscriber */

struct hwreg_event_msg {
	mach_msg_header_t	hdr;
	char			kind;
	char			text[479];
};

int
main(void)
{
	mach_port_t svc = MACH_PORT_NULL, notify = MACH_PORT_NULL;
	kern_return_t kr;
	mach_msg_return_t mr;
	mach_msg_header_t req;
	/*
	 * Receive buffer for the ack EVENT. A Mach receive needs room for
	 * the message *and* the trailer the kernel appends after it; a
	 * buffer sized at exactly sizeof(struct hwreg_event_msg) gets
	 * MACH_RCV_TOO_LARGE (0x10004004). Pad with a max trailer.
	 */
	struct {
		struct hwreg_event_msg	ev;
		mach_msg_max_trailer_t	trailer;
	} rcv;

	kr = bootstrap_look_up(bootstrap_port, "org.freebsd.hwregd", &svc);
	if (kr != KERN_SUCCESS) {
		printf("HWREG-PUBSUB-FAIL: bootstrap_look_up: 0x%x\n",
		    (unsigned)kr);
		return 1;
	}

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
	    &notify);
	if (kr != KERN_SUCCESS) {
		printf("HWREG-PUBSUB-FAIL: mach_port_allocate: 0x%x\n",
		    (unsigned)kr);
		return 1;
	}
	kr = mach_port_insert_right(mach_task_self(), notify, notify,
	    MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("HWREG-PUBSUB-FAIL: mach_port_insert_right: 0x%x\n",
		    (unsigned)kr);
		return 1;
	}

	/*
	 * SUBSCRIBE: remote = hwregd's service port, local = our notify
	 * port with MAKE_SEND — so hwregd receives a send right to our
	 * notify port as the message's remote port and can push to it.
	 */
	memset(&req, 0, sizeof(req));
	req.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
	    MACH_MSG_TYPE_MAKE_SEND);
	req.msgh_size = sizeof(req);
	req.msgh_remote_port = svc;
	req.msgh_local_port = notify;
	req.msgh_id = HWREG_MSG_SUBSCRIBE;
	mr = mach_msg(&req, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(req),
	    0, MACH_PORT_NULL, 2000, MACH_PORT_NULL);
	if (mr != MACH_MSG_SUCCESS) {
		printf("HWREG-PUBSUB-FAIL: subscribe send: 0x%x\n",
		    (unsigned)mr);
		return 1;
	}

	/* Receive the subscription-ack EVENT hwregd sends back. */
	memset(&rcv, 0, sizeof(rcv));
	mr = mach_msg(&rcv.ev.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
	    sizeof(rcv), notify, 5000, MACH_PORT_NULL);
	if (mr != MACH_MSG_SUCCESS) {
		printf("HWREG-PUBSUB-FAIL: ack receive: 0x%x\n", (unsigned)mr);
		return 1;
	}
	if (rcv.ev.hdr.msgh_id != HWREG_MSG_EVENT) {
		printf("HWREG-PUBSUB-FAIL: unexpected msg id=%d\n",
		    rcv.ev.hdr.msgh_id);
		return 1;
	}
	rcv.ev.text[sizeof(rcv.ev.text) - 1] = '\0';
	printf("HWREG-PUBSUB-OK: event kind=%c text=[%s]\n",
	    rcv.ev.kind ? rcv.ev.kind : '?', rcv.ev.text);
	return 0;
}
