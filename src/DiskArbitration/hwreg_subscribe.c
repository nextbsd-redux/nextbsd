/*
 * hwreg_subscribe.c — DiskArbitration iter 2 hwregd pub/sub client.
 *
 * Subscribe to org.freebsd.hwregd via the raw HWREG_MSG_SUBSCRIBE
 * protocol (same wire shape as src/hwregd/hwregtest.c). Receive
 * EVENTs in a background pthread, log every event, tag storage-
 * looking names (ada*, da*, nvd*, cd*, mmcsd*) with STORAGE.
 *
 * iter 2 deliberately uses the raw protocol rather than the MIG-
 * served hwreg_watch / hwreg_lookup — no MIG client stubs to vendor
 * yet, no nvlist criteria packing. iter 3 moves to MIG so we can
 * filter at the hwregd side (class="device") + enumerate the current
 * registry state at startup.
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
 * Wire protocol mirrors src/hwregd/hwregd.c + src/hwregd/hwregtest.c.
 * Keep these in sync if hwregd's protocol bumps.
 */
#define HWREG_MSG_SUBSCRIBE	0x48575201
#define HWREG_MSG_EVENT		0x48575202

struct hwreg_event_msg {
	mach_msg_header_t	hdr;
	char			kind;	/* '+' arrived, '-' departed */
	char			text[479];
};

static mach_port_t	g_notify_port = MACH_PORT_NULL;
static pthread_t	g_recv_thread;

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "diskarbitrationd[hwreg] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/*
 * "arrived id=N name=ada0 class=PCIDevice" or "departed id=N name=..."
 * is the upstream format from hwregd's notify_watchers. Pull the name
 * out by scanning for "name=" and the next space. Returns "" if not
 * present. Output buffer is the caller's; sized 32 in practice (matches
 * hwregd's want_name limit).
 */
static void
parse_devname(const char *text, char *out, size_t out_len)
{
	const char *p, *q;
	size_t n;

	out[0] = '\0';
	p = strstr(text, "name=");
	if (p == NULL)
		return;
	p += 5;	/* skip "name=" */
	q = strchr(p, ' ');
	n = (q != NULL) ? (size_t)(q - p) : strlen(p);
	if (n >= out_len)
		n = out_len - 1;
	(void)memcpy(out, p, n);
	out[n] = '\0';
}

/*
 * True if `name` looks like a FreeBSD storage device that DA cares
 * about: ada* (AHCI/CAM ATA), da* (SCSI / USB), nvd* / nda* (NVMe),
 * cd* (optical), mmcsd* (SD/eMMC). iter 3 will replace this with a
 * registry class filter via hwreg_watch.
 */
static int
is_storage_name(const char *name)
{
	static const char *prefixes[] = {
		"ada", "da", "nvd", "nda", "cd", "mmcsd"
	};
	size_t i;

	for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		size_t plen = strlen(prefixes[i]);
		if (strncmp(name, prefixes[i], plen) != 0)
			continue;
		if (name[plen] >= '0' && name[plen] <= '9')
			return (1);
	}
	return (0);
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
		int is_storage;

		(void)memset(&rcv, 0, sizeof(rcv));
		mr = mach_msg(&rcv.ev.hdr, MACH_RCV_MSG, 0, sizeof(rcv),
		    g_notify_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
		if (mr != MACH_MSG_SUCCESS) {
			xlog("recv loop: mach_msg 0x%x — exiting thread",
			    (unsigned)mr);
			return (NULL);
		}
		if (rcv.ev.hdr.msgh_id != HWREG_MSG_EVENT) {
			xlog("recv loop: unexpected msgh_id=%d",
			    (int)rcv.ev.hdr.msgh_id);
			continue;
		}

		rcv.ev.text[sizeof(rcv.ev.text) - 1] = '\0';
		parse_devname(rcv.ev.text, name, sizeof(name));
		is_storage = (name[0] != '\0') && is_storage_name(name);

		xlog("%s kind=%c %s", is_storage ? "STORAGE" : "event",
		    rcv.ev.kind ? rcv.ev.kind : '?', rcv.ev.text);
	}
}

int
hwreg_subscribe_start(void)
{
	mach_port_t svc = MACH_PORT_NULL;
	kern_return_t kr;
	mach_msg_return_t mr;
	mach_msg_header_t req;
	struct {
		struct hwreg_event_msg	ev;
		mach_msg_max_trailer_t	trailer;
	} rcv;

	kr = bootstrap_look_up(bootstrap_port, "org.freebsd.hwregd", &svc);
	if (kr != KERN_SUCCESS) {
		xlog("DA-WATCH-FAIL: bootstrap_look_up(org.freebsd.hwregd): "
		    "0x%x", (unsigned)kr);
		return (-1);
	}

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
	    &g_notify_port);
	if (kr != KERN_SUCCESS) {
		xlog("DA-WATCH-FAIL: mach_port_allocate: 0x%x", (unsigned)kr);
		return (-1);
	}
	kr = mach_port_insert_right(mach_task_self(), g_notify_port,
	    g_notify_port, MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		xlog("DA-WATCH-FAIL: mach_port_insert_right: 0x%x",
		    (unsigned)kr);
		return (-1);
	}

	/*
	 * SUBSCRIBE: remote = hwregd's service port, local = our notify
	 * port with MAKE_SEND — hwregd receives a send right and can
	 * push EVENTs to us. Same shape hwregtest.c uses.
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
		xlog("DA-WATCH-FAIL: SUBSCRIBE send: 0x%x", (unsigned)mr);
		return (-1);
	}

	/* Receive the subscription-ack EVENT hwregd sends back. */
	(void)memset(&rcv, 0, sizeof(rcv));
	mr = mach_msg(&rcv.ev.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
	    sizeof(rcv), g_notify_port, 5000, MACH_PORT_NULL);
	if (mr != MACH_MSG_SUCCESS) {
		xlog("DA-WATCH-FAIL: ack receive: 0x%x", (unsigned)mr);
		return (-1);
	}
	if (rcv.ev.hdr.msgh_id != HWREG_MSG_EVENT) {
		xlog("DA-WATCH-FAIL: unexpected ack msgh_id=%d",
		    (int)rcv.ev.hdr.msgh_id);
		return (-1);
	}
	rcv.ev.text[sizeof(rcv.ev.text) - 1] = '\0';
	xlog("DA-WATCH-OK: subscribed to org.freebsd.hwregd "
	    "(ack kind=%c text=[%s])",
	    rcv.ev.kind ? rcv.ev.kind : '?', rcv.ev.text);

	if (pthread_create(&g_recv_thread, NULL, recv_loop, NULL) != 0) {
		xlog("DA-WATCH-WARN: pthread_create failed: %s — "
		    "subscription is up but events won't be logged",
		    strerror(errno));
		/* still considered success — the subscription itself works */
	}
	return (0);
}
