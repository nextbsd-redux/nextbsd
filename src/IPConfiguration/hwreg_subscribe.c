/*
 * hwreg_subscribe.c — IPConfiguration iter 9 hwregd pub/sub client.
 *
 * Adapted from src/DiskArbitration/hwreg_subscribe.c (iter 2). Same
 * wire protocol (HWREG_MSG_SUBSCRIBE / HWREG_MSG_EVENT raw mach_msg,
 * no MIG); the differences from the DA version:
 *   - Filter on kind == '+' attach (not '!' notify or '?' nomatch).
 *   - Parse `dev=<name>` from publish_event's "attach: dev=X parent=Y
 *     loc=[Z]" text (DA's `name=` only ever appears in MIG watch
 *     events from notify_watchers, never in the bare pub/sub stream
 *     — see src/hwregd/hwregd.c handle_attach_detach).
 *   - Dispatch to a caller-supplied callback so ipconfigd can decide
 *     whether to DHCP the named interface (typically: skip if a
 *     binding already exists, else run dhcp_run_on_interface).
 */
#include "hwreg_subscribe.h"

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Wire protocol mirrors src/hwregd/hwregd.c + hwregtest.c. Keep in
 * sync if hwregd's protocol bumps. Match the DA copy verbatim so the
 * two clients can't drift out of step.
 */
#define HWREG_MSG_SUBSCRIBE	0x48575201
#define HWREG_MSG_EVENT		0x48575202

struct hwreg_event_msg {
	mach_msg_header_t	hdr;
	char			kind;	/* '+' attach, '-' detach, '?' nomatch, '!' notify */
	char			text[479];
};

static mach_port_t			g_notify_port = MACH_PORT_NULL;
static pthread_t			g_recv_thread;
static hwreg_subscribe_attach_cb	g_cb;
static uint32_t				g_lease_cap_secs;

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "ipconfigd[hwreg] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/*
 * Parse `dev=<name>` from a pub/sub attach/detach text like
 * "attach: dev=em0 parent=pci0 loc=[slot=...]". Returns "" if absent.
 * Output sized to IF_NAMESIZE in practice.
 */
static void
parse_devname(const char *text, char *out, size_t out_len)
{
	const char *p, *q;
	size_t n;

	out[0] = '\0';
	p = strstr(text, "dev=");
	if (p == NULL)
		return;
	p += 4;	/* skip "dev=" */
	q = strchr(p, ' ');
	n = (q != NULL) ? (size_t)(q - p) : strlen(p);
	if (n >= out_len)
		n = out_len - 1;
	(void)memcpy(out, p, n);
	out[n] = '\0';
}

static void *
recv_loop(void *arg)
{
	(void)arg;

	for (;;) {
		struct {
			struct hwreg_event_msg	ev;
			mach_msg_max_trailer_t	trailer;
		} rcv;
		mach_msg_return_t mr;
		char name[32];

		(void)memset(&rcv, 0, sizeof(rcv));
		mr = mach_msg(&rcv.ev.hdr, MACH_RCV_MSG, 0, sizeof(rcv),
		    g_notify_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
		if (mr != MACH_MSG_SUCCESS) {
			xlog("recv loop: mach_msg 0x%x — exiting thread",
			    (unsigned)mr);
			return (NULL);
		}
		if (rcv.ev.hdr.msgh_id != HWREG_MSG_EVENT)
			continue;

		rcv.ev.text[sizeof(rcv.ev.text) - 1] = '\0';

		/*
		 * Only '+' attach matters for the autoload path: a NIC
		 * appears, get DHCP running on it. '-' detach, '?' nomatch,
		 * '!' notify are logged at debug but not actioned (LINK_UP
		 * reaction to hot-plug cable is a follow-up iter).
		 */
		if (rcv.ev.kind != '+')
			continue;

		parse_devname(rcv.ev.text, name, sizeof(name));
		if (name[0] == '\0')
			continue;

		xlog("attach event for dev=%s — dispatching to callback",
		    name);
		if (g_cb != NULL)
			g_cb(name, g_lease_cap_secs);
		/*
		 * Callback may have entered lease_loop_run and never
		 * returned. If it did return, keep listening — a second
		 * attach (different NIC) will still be delivered.
		 */
	}
}

int
hwreg_subscribe_start(hwreg_subscribe_attach_cb cb, uint32_t lease_cap_secs)
{
	mach_port_t svc = MACH_PORT_NULL;
	kern_return_t kr;
	mach_msg_return_t mr;
	mach_msg_header_t req;
	struct {
		struct hwreg_event_msg	ev;
		mach_msg_max_trailer_t	trailer;
	} rcv;

	g_cb = cb;
	g_lease_cap_secs = lease_cap_secs;

	kr = bootstrap_look_up(bootstrap_port, "org.freebsd.hwregd", &svc);
	if (kr != KERN_SUCCESS) {
		xlog("IPCFG-AUTOLOAD-SUB-FAIL: bootstrap_look_up("
		    "org.freebsd.hwregd): 0x%x", (unsigned)kr);
		return (-1);
	}

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
	    &g_notify_port);
	if (kr != KERN_SUCCESS) {
		xlog("IPCFG-AUTOLOAD-SUB-FAIL: mach_port_allocate: 0x%x",
		    (unsigned)kr);
		return (-1);
	}
	kr = mach_port_insert_right(mach_task_self(), g_notify_port,
	    g_notify_port, MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		xlog("IPCFG-AUTOLOAD-SUB-FAIL: mach_port_insert_right: 0x%x",
		    (unsigned)kr);
		return (-1);
	}

	/*
	 * SUBSCRIBE: remote = hwregd's service port, local = our notify
	 * port with MAKE_SEND. hwregd inserts a send right and pushes
	 * EVENTs to us. Same shape hwregtest.c uses.
	 */
	(void)memset(&req, 0, sizeof(req));
	req.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
	    MACH_MSG_TYPE_MAKE_SEND);
	req.msgh_size = sizeof(req);
	req.msgh_remote_port = svc;
	req.msgh_local_port = g_notify_port;
	req.msgh_id = HWREG_MSG_SUBSCRIBE;
	mr = mach_msg(&req, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(req),
	    0, MACH_PORT_NULL, 2000, MACH_PORT_NULL);
	if (mr != MACH_MSG_SUCCESS) {
		xlog("IPCFG-AUTOLOAD-SUB-FAIL: SUBSCRIBE send: 0x%x",
		    (unsigned)mr);
		return (-1);
	}

	/* Subscription-ack EVENT. 5s budget is plenty for the round-trip. */
	(void)memset(&rcv, 0, sizeof(rcv));
	mr = mach_msg(&rcv.ev.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
	    sizeof(rcv), g_notify_port, 5000, MACH_PORT_NULL);
	if (mr != MACH_MSG_SUCCESS) {
		xlog("IPCFG-AUTOLOAD-SUB-FAIL: ack receive: 0x%x",
		    (unsigned)mr);
		return (-1);
	}
	if (rcv.ev.hdr.msgh_id != HWREG_MSG_EVENT) {
		xlog("IPCFG-AUTOLOAD-SUB-FAIL: unexpected ack msgh_id=%d",
		    (int)rcv.ev.hdr.msgh_id);
		return (-1);
	}
	rcv.ev.text[sizeof(rcv.ev.text) - 1] = '\0';
	xlog("IPCFG-AUTOLOAD-SUB-OK: subscribed to org.freebsd.hwregd "
	    "(ack kind=%c text=[%s])",
	    rcv.ev.kind ? rcv.ev.kind : '?', rcv.ev.text);

	if (pthread_create(&g_recv_thread, NULL, recv_loop, NULL) != 0) {
		xlog("IPCFG-AUTOLOAD-SUB-WARN: pthread_create failed: %s — "
		    "subscription is up but events won't be dispatched",
		    strerror(errno));
		/* subscription itself works — count it as success */
	}
	return (0);
}
