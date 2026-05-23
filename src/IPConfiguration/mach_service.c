/*
 * mach_service.c — ipconfigd's Mach service thread.
 *
 * Owns the receive right for com.apple.IPConfiguration. Runs a raw
 * mach_msg(MACH_RCV_MSG | MACH_RCV_TIMEOUT, 500ms) loop and hands
 * each message to _ipconfig_server() (the MIG demux for
 * ipconfig.defs). Same shape as configd_serve() / hwregd's
 * mach_service_thread().
 *
 * 500ms receive timeout: keeps the loop awake to notice got_term
 * for a clean shutdown.
 */
#include "mach_service.h"
#include "dhcp_discover.h"		/* got_term */
#include "ipconfigServer.h"		/* MIG: _ipconfig_server() */

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* RPC receive buffer size — Apple's ipconfig.defs has no OOL data
 * in iter 5a (just scalars + a 16-byte if_name), so a 4 KiB buffer
 * is generous. iter 5b/6's xmlDataOut routines may need 8 KiB+. */
#define IPCFG_RPC_BUFSZ		4096

#define IPCFG_SERVICE_NAME	"com.apple.IPConfiguration"

static pthread_t	worker;
static int		worker_running;

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "ipconfigd[mach] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

static void *
mach_service_thread(void *arg)
{
	mach_port_t sp = MACH_PORT_NULL;
	kern_return_t kr;

	(void)arg;

	kr = bootstrap_check_in(bootstrap_port, IPCFG_SERVICE_NAME, &sp);
	if (kr != KERN_SUCCESS) {
		xlog("bootstrap_check_in('%s') failed: 0x%x — RPC off",
		    IPCFG_SERVICE_NAME, (unsigned)kr);
		return (NULL);
	}
	xlog("Mach service '%s' checked in (port=0x%x)",
	    IPCFG_SERVICE_NAME, (unsigned)sp);

	while (!got_term) {
		union {
			mach_msg_header_t	hdr;
			char			buf[IPCFG_RPC_BUFSZ];
		} req, rep;
		mach_msg_return_t mr;

		(void)memset(&req, 0, sizeof(req));
		mr = mach_msg(&req.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
		    0, sizeof(req), sp, 500, MACH_PORT_NULL);
		if (mr == MACH_RCV_TIMED_OUT)
			continue;
		if (mr != MACH_MSG_SUCCESS) {
			xlog("receive failed: 0x%x — service off",
			    (unsigned)mr);
			break;
		}

		(void)memset(&rep, 0, sizeof(rep));
		if (!_ipconfig_server(&req.hdr, &rep.hdr)) {
			xlog("ignoring message id=%d", req.hdr.msgh_id);
			continue;
		}
		if (rep.hdr.msgh_remote_port != MACH_PORT_NULL) {
			mr = mach_msg(&rep.hdr,
			    MACH_SEND_MSG | MACH_SEND_TIMEOUT,
			    rep.hdr.msgh_size, 0, MACH_PORT_NULL, 200,
			    MACH_PORT_NULL);
			if (mr != MACH_MSG_SUCCESS)
				xlog("reply send failed: 0x%x",
				    (unsigned)mr);
		}
	}
	return (NULL);
}

int
mach_service_start(void)
{
	int rc;

	if (worker_running)
		return (0);
	rc = pthread_create(&worker, NULL, mach_service_thread, NULL);
	if (rc != 0) {
		xlog("pthread_create failed: %s", strerror(rc));
		return (-1);
	}
	worker_running = 1;
	return (0);
}

void
mach_service_join(void)
{
	if (!worker_running)
		return;
	(void)pthread_join(worker, NULL);
	worker_running = 0;
}
