/*
 * configd.c — SCDynamicStore configuration daemon (freebsd-launchd-mach
 * port), iteration 1: the daemon skeleton.
 *
 * Apple's configd hosts the SCDynamicStore — a system-wide key->plist
 * store with change notifications — over the MIG `config` subsystem
 * (config.defs, base id 20000). This port keeps that real Mach IPC
 * design (see the configd-port memory / plan).
 *
 * iter 1 brings up the daemon shell only: check the bootstrap service
 * in with launchd and run a raw mach_msg receive loop that hands each
 * message to the MIG-generated config_server() demux. The routine
 * handlers (_configopen ... _snapshot) are stubs here — they return a
 * "not implemented" status with all out-parameters zeroed. iter 2
 * replaces them with the real store engine (session.c + _SCD.c).
 *
 * Apple's configd.tproj drives IPC through libdispatch's dispatch_mach
 * channel API; that API is unfinished in this port, so configd uses a
 * raw mach_msg loop instead — the same proven pattern hwregd and
 * launchd use here. configd is a single-purpose server, so its main
 * thread is the receive loop (no worker thread needed).
 */

#include <sys/types.h>

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config_types.h"
#include "configServer.h"	/* MIG: config_server() demux + _config* protos */

/*
 * SCDynamicStore status returned by the iter-1 stub handlers. 1001 is
 * Apple's kSCStatusFailed; iter 2 pulls in the real SC status codes.
 */
#define SCD_STATUS_NOTYET	1001

/* Inline message buffer. config.defs payloads (xmlData) travel as
 * out-of-line descriptors, so the inline message is small — header,
 * a couple of descriptors, scalars, trailer. 8 KiB is ample. */
#define CONFIGD_MSG_BUFSZ	8192

static volatile sig_atomic_t	got_term;

static void
on_signal(int sig)
{
	got_term = sig;
}

/* Timestamped diagnostic line to stderr (the configd launchd plist
 * routes stderr to /var/log/configd.stderr, the CI boot test's path). */
static void
clog(const char *fmt, ...)
{
	struct timespec ts;
	struct tm tm;
	char tbuf[32];
	va_list ap;

	(void)clock_gettime(CLOCK_REALTIME, &ts);
	(void)gmtime_r(&ts.tv_sec, &tm);
	(void)strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", &tm);
	(void)fprintf(stderr, "configd %s ", tbuf);
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/*
 * MIG routine handlers — config_server() dispatches to these. iter 1
 * stubs: every out-parameter is given a safe zero value (so the
 * MIG-generated reply marshalling never sends an uninitialised port
 * or out-of-line pointer) and the SCDynamicStore status reports
 * "not implemented". Real implementations land in iter 2.
 */

kern_return_t
_configopen(mach_port_t server, xmlData_t name, mach_msg_type_number_t nameCnt,
    xmlData_t options, mach_msg_type_number_t optionsCnt,
    mach_port_t *session, int *status)
{
	(void)server; (void)name; (void)nameCnt;
	(void)options; (void)optionsCnt;
	*session = MACH_PORT_NULL;
	*status = SCD_STATUS_NOTYET;
	return KERN_SUCCESS;
}

kern_return_t
_configlist(mach_port_t server, xmlData_t key, mach_msg_type_number_t keyCnt,
    int isRegex, xmlDataOut_t *list, mach_msg_type_number_t *listCnt,
    int *status)
{
	(void)server; (void)key; (void)keyCnt; (void)isRegex;
	*list = NULL;
	*listCnt = 0;
	*status = SCD_STATUS_NOTYET;
	return KERN_SUCCESS;
}

kern_return_t
_configadd(mach_port_t server, xmlData_t key, mach_msg_type_number_t keyCnt,
    xmlData_t data, mach_msg_type_number_t dataCnt, int *newInstance,
    int *status)
{
	(void)server; (void)key; (void)keyCnt; (void)data; (void)dataCnt;
	*newInstance = 0;
	*status = SCD_STATUS_NOTYET;
	return KERN_SUCCESS;
}

kern_return_t
_configget(mach_port_t server, xmlData_t key, mach_msg_type_number_t keyCnt,
    xmlDataOut_t *data, mach_msg_type_number_t *dataCnt, int *newInstance,
    int *status)
{
	(void)server; (void)key; (void)keyCnt;
	*data = NULL;
	*dataCnt = 0;
	*newInstance = 0;
	*status = SCD_STATUS_NOTYET;
	return KERN_SUCCESS;
}

kern_return_t
_configset(mach_port_t server, xmlData_t key, mach_msg_type_number_t keyCnt,
    xmlData_t data, mach_msg_type_number_t dataCnt, int instance,
    int *newInstance, int *status)
{
	(void)server; (void)key; (void)keyCnt; (void)data; (void)dataCnt;
	(void)instance;
	*newInstance = 0;
	*status = SCD_STATUS_NOTYET;
	return KERN_SUCCESS;
}

kern_return_t
_configremove(mach_port_t server, xmlData_t key, mach_msg_type_number_t keyCnt,
    int *status)
{
	(void)server; (void)key; (void)keyCnt;
	*status = SCD_STATUS_NOTYET;
	return KERN_SUCCESS;
}

kern_return_t
_configadd_s(mach_port_t server, xmlData_t key, mach_msg_type_number_t keyCnt,
    xmlData_t data, mach_msg_type_number_t dataCnt, int *newInstance,
    int *status)
{
	(void)server; (void)key; (void)keyCnt; (void)data; (void)dataCnt;
	*newInstance = 0;
	*status = SCD_STATUS_NOTYET;
	return KERN_SUCCESS;
}

kern_return_t
_confignotify(mach_port_t server, xmlData_t key, mach_msg_type_number_t keyCnt,
    int *status)
{
	(void)server; (void)key; (void)keyCnt;
	*status = SCD_STATUS_NOTYET;
	return KERN_SUCCESS;
}

kern_return_t
_configget_m(mach_port_t server, xmlData_t keys, mach_msg_type_number_t keysCnt,
    xmlData_t patterns, mach_msg_type_number_t patternsCnt,
    xmlDataOut_t *data, mach_msg_type_number_t *dataCnt, int *status)
{
	(void)server; (void)keys; (void)keysCnt;
	(void)patterns; (void)patternsCnt;
	*data = NULL;
	*dataCnt = 0;
	*status = SCD_STATUS_NOTYET;
	return KERN_SUCCESS;
}

kern_return_t
_configset_m(mach_port_t server, xmlData_t data, mach_msg_type_number_t dataCnt,
    xmlData_t removeData, mach_msg_type_number_t removeCnt,
    xmlData_t notify, mach_msg_type_number_t notifyCnt, int *status)
{
	(void)server; (void)data; (void)dataCnt;
	(void)removeData; (void)removeCnt; (void)notify; (void)notifyCnt;
	*status = SCD_STATUS_NOTYET;
	return KERN_SUCCESS;
}

kern_return_t
_notifyadd(mach_port_t server, xmlData_t key, mach_msg_type_number_t keyCnt,
    int isRegex, int *status)
{
	(void)server; (void)key; (void)keyCnt; (void)isRegex;
	*status = SCD_STATUS_NOTYET;
	return KERN_SUCCESS;
}

kern_return_t
_notifyremove(mach_port_t server, xmlData_t key, mach_msg_type_number_t keyCnt,
    int isRegex, int *status)
{
	(void)server; (void)key; (void)keyCnt; (void)isRegex;
	*status = SCD_STATUS_NOTYET;
	return KERN_SUCCESS;
}

kern_return_t
_notifychanges(mach_port_t server, xmlDataOut_t *list,
    mach_msg_type_number_t *listCnt, int *status)
{
	(void)server;
	*list = NULL;
	*listCnt = 0;
	*status = SCD_STATUS_NOTYET;
	return KERN_SUCCESS;
}

kern_return_t
_notifyviaport(mach_port_t server, mach_port_t port, mach_msg_id_t msgid,
    int *status)
{
	(void)server; (void)port; (void)msgid;
	*status = SCD_STATUS_NOTYET;
	return KERN_SUCCESS;
}

kern_return_t
_notifycancel(mach_port_t server, int *status)
{
	(void)server;
	*status = SCD_STATUS_NOTYET;
	return KERN_SUCCESS;
}

kern_return_t
_notifyset(mach_port_t server, xmlData_t keys, mach_msg_type_number_t keysCnt,
    xmlData_t patterns, mach_msg_type_number_t patternsCnt, int *status)
{
	(void)server; (void)keys; (void)keysCnt;
	(void)patterns; (void)patternsCnt;
	*status = SCD_STATUS_NOTYET;
	return KERN_SUCCESS;
}

kern_return_t
_notifyviafd(mach_port_t server, mach_port_t fileport, int identifier,
    int *status)
{
	(void)server; (void)fileport; (void)identifier;
	*status = SCD_STATUS_NOTYET;
	return KERN_SUCCESS;
}

kern_return_t
_snapshot(mach_port_t server, int *status)
{
	(void)server;
	*status = SCD_STATUS_NOTYET;
	return KERN_SUCCESS;
}

int
main(int argc, char **argv)
{
	mach_port_t	service_port = MACH_PORT_NULL;
	kern_return_t	kr;
	const char	*service_name = SCD_SERVER;

	(void)argc;
	(void)argv;

	(void)signal(SIGTERM, on_signal);
	(void)signal(SIGINT, on_signal);

	/*
	 * Check the SCDynamicStore service in with launchd. configd is a
	 * launchd job whose plist declares this MachService, so launchd
	 * created the port and hands us the receive right here.
	 */
	kr = bootstrap_check_in(bootstrap_port, service_name, &service_port);
	if (kr != BOOTSTRAP_SUCCESS) {
		clog("bootstrap_check_in(%s) failed: 0x%x — exiting",
		    service_name, (unsigned)kr);
		return 1;
	}
	clog("Mach service '%s' checked in (port=0x%x)",
	    service_name, (unsigned)service_port);

	/*
	 * Raw mach_msg receive loop. config_server() is the MIG demux for
	 * the config subsystem; it writes the routine's reply (or a MIG
	 * error) into `rep`. The 1s receive timeout lets the loop notice
	 * a SIGTERM for a clean shutdown.
	 */
	while (!got_term) {
		union {
			mach_msg_header_t hdr;
			char buf[CONFIGD_MSG_BUFSZ];
		} req, rep;
		mach_msg_return_t mr;

		memset(&req, 0, sizeof(req));
		mr = mach_msg(&req.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
		    sizeof(req), service_port, 1000, MACH_PORT_NULL);
		if (mr == MACH_RCV_TIMED_OUT)
			continue;
		if (mr != MACH_MSG_SUCCESS) {
			clog("mach_msg receive failed: 0x%x — exiting",
			    (unsigned)mr);
			break;
		}

		memset(&rep, 0, sizeof(rep));
		if (!config_server(&req.hdr, &rep.hdr)) {
			clog("unhandled message id=%d", req.hdr.msgh_id);
			continue;
		}
		if (rep.hdr.msgh_remote_port != MACH_PORT_NULL) {
			mr = mach_msg(&rep.hdr, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
			    rep.hdr.msgh_size, 0, MACH_PORT_NULL, 1000,
			    MACH_PORT_NULL);
			if (mr != MACH_MSG_SUCCESS)
				clog("reply send failed: 0x%x", (unsigned)mr);
		}
	}

	clog("shutting down (signal %d)", (int)got_term);
	return 0;
}
