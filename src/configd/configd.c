/*
 * configd.c — SCDynamicStore configuration daemon (freebsd-launchd-mach
 * port).
 *
 * Apple's configd hosts the SCDynamicStore — a system-wide key->plist
 * store with change notifications — over the MIG `config` subsystem
 * (config.defs, base id 20000). This port keeps that real Mach IPC
 * design (see the configd-port memory / plan).
 *
 * The daemon checks the bootstrap service in with launchd and runs a
 * raw mach_msg receive loop that hands each message to the
 * MIG-generated config_server() demux. Apple's configd.tproj drives
 * IPC through libdispatch's dispatch_mach channel API; that API is
 * unfinished in this port, so configd uses a raw mach_msg loop
 * instead — the same proven pattern hwregd and launchd use here.
 * configd is a single-purpose server, so its main thread is the
 * receive loop (no worker thread needed).
 *
 * configopen / configget / configset / configremove are implemented
 * against config_store.c; configlist, the add / multi variants, the
 * notify* routines and snapshot are still stubs (later iterations).
 * config.defs carries the key/value payloads INLINE in bounded
 * arrays — out-of-line Mach data is broken cross-process in this
 * repo's kernel (see config.defs) — so a handler's xmlData argument
 * is the byte buffer itself; there is no out-of-line memory to free.
 *
 * `configd --selftest` runs an in-process round trip (a server thread
 * plus the client stubs) — no launchd or bootstrap — as a quick
 * self-check of the config.defs RPC path.
 */

#include <sys/types.h>

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config_types.h"
#include "config_store.h"
#include "configServer.h"	/* MIG: config_server() demux + _config* protos */

/*
 * SCDynamicStore status codes — a subset of Apple's SCError.h. configd
 * and the SystemConfiguration client must agree on these values: they
 * cross the wire as the `status` reply field.
 */
#define kSCStatusOK			0
#define kSCStatusFailed			1001
#define kSCStatusInvalidArgument	1002
#define kSCStatusNoKey			1004

/* Maximum key / value size — config.defs' array[*:8192] bound. */
#define CONFIG_DATA_MAX			8192

/*
 * Inline message buffer. config.defs carries the xmlData payloads
 * inline in bounded arrays, so a request can be large — configset_m
 * has three. 32 KiB covers the largest config message.
 */
#define CONFIGD_MSG_BUFSZ		32768

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
 * MIG routine handlers — config_server() dispatches to these. An
 * xmlData / xmlDataOut argument is an inline byte buffer (config.defs
 * carries the payloads inline); the matching mach_msg_type_number_t
 * carries its length. Out-parameters are given a safe value first so
 * the MIG reply marshalling never ships an uninitialised port or
 * count. configlist, the add / multi variants and every notify* are
 * still stubs returning kSCStatusFailed.
 */

kern_return_t
_configopen(mach_port_t server, xmlData name, mach_msg_type_number_t nameCnt,
    xmlData options, mach_msg_type_number_t optionsCnt,
    mach_port_t *session, int *status)
{
	/*
	 * iter 2 session model: hand the client a send right to this
	 * same service port — per-session ports arrive with change
	 * notifications. Insert a MAKE_SEND right onto the receive-right
	 * name; MIG's mach_port_move_send_t moves it out in the reply.
	 */
	(void)name;
	(void)nameCnt;
	(void)options;
	(void)optionsCnt;

	if (mach_port_insert_right(mach_task_self(), server, server,
	    MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS) {
		*session = MACH_PORT_NULL;
		*status = kSCStatusFailed;
		return KERN_SUCCESS;
	}
	*session = server;
	*status = kSCStatusOK;
	return KERN_SUCCESS;
}

kern_return_t
_configlist(mach_port_t server, xmlData key, mach_msg_type_number_t keyCnt,
    int isRegex, xmlDataOut list, mach_msg_type_number_t *listCnt,
    int *status)
{
	(void)server; (void)key; (void)keyCnt; (void)isRegex; (void)list;
	*listCnt = 0;
	*status = kSCStatusFailed;
	return KERN_SUCCESS;
}

kern_return_t
_configadd(mach_port_t server, xmlData key, mach_msg_type_number_t keyCnt,
    xmlData data, mach_msg_type_number_t dataCnt, int *newInstance,
    int *status)
{
	(void)server; (void)key; (void)keyCnt; (void)data; (void)dataCnt;
	*newInstance = 0;
	*status = kSCStatusFailed;
	return KERN_SUCCESS;
}

kern_return_t
_configget(mach_port_t server, xmlData key, mach_msg_type_number_t keyCnt,
    xmlDataOut data, mach_msg_type_number_t *dataCnt, int *newInstance,
    int *status)
{
	const void	*val;
	size_t		vlen;

	(void)server;
	*dataCnt = 0;
	*newInstance = 0;

	if (keyCnt == 0) {
		*status = kSCStatusInvalidArgument;
	} else if (store_get(key, keyCnt, &val, &vlen) != 0) {
		*status = kSCStatusNoKey;
	} else if (vlen > CONFIG_DATA_MAX) {
		/* a stored value too large for the inline reply array */
		*status = kSCStatusFailed;
	} else {
		if (vlen != 0)
			memcpy(data, val, vlen);
		*dataCnt = (mach_msg_type_number_t)vlen;
		*status = kSCStatusOK;
	}
	return KERN_SUCCESS;
}

kern_return_t
_configset(mach_port_t server, xmlData key, mach_msg_type_number_t keyCnt,
    xmlData data, mach_msg_type_number_t dataCnt, int instance,
    int *newInstance, int *status)
{
	(void)server;
	(void)instance;		/* the "instance" generation count is unused */
	*newInstance = 0;

	if (keyCnt == 0)
		*status = kSCStatusInvalidArgument;
	else if (store_set(key, keyCnt, data, dataCnt) != 0)
		*status = kSCStatusFailed;
	else
		*status = kSCStatusOK;
	return KERN_SUCCESS;
}

kern_return_t
_configremove(mach_port_t server, xmlData key, mach_msg_type_number_t keyCnt,
    int *status)
{
	(void)server;

	if (keyCnt == 0)
		*status = kSCStatusInvalidArgument;
	else if (store_remove(key, keyCnt) != 0)
		*status = kSCStatusNoKey;
	else
		*status = kSCStatusOK;
	return KERN_SUCCESS;
}

kern_return_t
_configadd_s(mach_port_t server, xmlData key, mach_msg_type_number_t keyCnt,
    xmlData data, mach_msg_type_number_t dataCnt, int *newInstance,
    int *status)
{
	(void)server; (void)key; (void)keyCnt; (void)data; (void)dataCnt;
	*newInstance = 0;
	*status = kSCStatusFailed;
	return KERN_SUCCESS;
}

kern_return_t
_confignotify(mach_port_t server, xmlData key, mach_msg_type_number_t keyCnt,
    int *status)
{
	(void)server; (void)key; (void)keyCnt;
	*status = kSCStatusFailed;
	return KERN_SUCCESS;
}

kern_return_t
_configget_m(mach_port_t server, xmlData keys, mach_msg_type_number_t keysCnt,
    xmlData patterns, mach_msg_type_number_t patternsCnt, xmlDataOut data,
    mach_msg_type_number_t *dataCnt, int *status)
{
	(void)server; (void)keys; (void)keysCnt;
	(void)patterns; (void)patternsCnt; (void)data;
	*dataCnt = 0;
	*status = kSCStatusFailed;
	return KERN_SUCCESS;
}

kern_return_t
_configset_m(mach_port_t server, xmlData data, mach_msg_type_number_t dataCnt,
    xmlData removeData, mach_msg_type_number_t removeCnt,
    xmlData notify, mach_msg_type_number_t notifyCnt, int *status)
{
	(void)server; (void)data; (void)dataCnt;
	(void)removeData; (void)removeCnt; (void)notify; (void)notifyCnt;
	*status = kSCStatusFailed;
	return KERN_SUCCESS;
}

kern_return_t
_notifyadd(mach_port_t server, xmlData key, mach_msg_type_number_t keyCnt,
    int isRegex, int *status)
{
	(void)server; (void)key; (void)keyCnt; (void)isRegex;
	*status = kSCStatusFailed;
	return KERN_SUCCESS;
}

kern_return_t
_notifyremove(mach_port_t server, xmlData key, mach_msg_type_number_t keyCnt,
    int isRegex, int *status)
{
	(void)server; (void)key; (void)keyCnt; (void)isRegex;
	*status = kSCStatusFailed;
	return KERN_SUCCESS;
}

kern_return_t
_notifychanges(mach_port_t server, xmlDataOut list,
    mach_msg_type_number_t *listCnt, int *status)
{
	(void)server; (void)list;
	*listCnt = 0;
	*status = kSCStatusFailed;
	return KERN_SUCCESS;
}

kern_return_t
_notifyviaport(mach_port_t server, mach_port_t port, mach_msg_id_t msgid,
    int *status)
{
	(void)server; (void)port; (void)msgid;
	*status = kSCStatusFailed;
	return KERN_SUCCESS;
}

kern_return_t
_notifycancel(mach_port_t server, int *status)
{
	(void)server;
	*status = kSCStatusFailed;
	return KERN_SUCCESS;
}

kern_return_t
_notifyset(mach_port_t server, xmlData keys, mach_msg_type_number_t keysCnt,
    xmlData patterns, mach_msg_type_number_t patternsCnt, int *status)
{
	(void)server; (void)keys; (void)keysCnt;
	(void)patterns; (void)patternsCnt;
	*status = kSCStatusFailed;
	return KERN_SUCCESS;
}

kern_return_t
_notifyviafd(mach_port_t server, mach_port_t fileport, int identifier,
    int *status)
{
	(void)server; (void)fileport; (void)identifier;
	*status = kSCStatusFailed;
	return KERN_SUCCESS;
}

kern_return_t
_snapshot(mach_port_t server, int *status)
{
	(void)server;
	*status = kSCStatusFailed;
	return KERN_SUCCESS;
}

/*
 * configd_serve — the raw mach_msg receive loop. config_server() is
 * the MIG demux for the config subsystem; it writes the routine's
 * reply (or a MIG error) into `rep`. The 1s receive timeout lets the
 * loop notice a SIGTERM for a clean shutdown.
 */
static void
configd_serve(mach_port_t service_port)
{
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
}

/*
 * In-process self test (`configd --selftest`). A server thread runs
 * configd_serve() on a private port; the main thread drives the
 * config.defs client stubs against it — configopen / configset /
 * configget — with no launchd or bootstrap in the loop. The client
 * stubs come from the MIG-generated configUser.c.
 */
extern kern_return_t configopen(mach_port_t, xmlData,
    mach_msg_type_number_t, xmlData, mach_msg_type_number_t,
    mach_port_t *, int *);
extern kern_return_t configset(mach_port_t, xmlData,
    mach_msg_type_number_t, xmlData, mach_msg_type_number_t, int,
    int *, int *);
extern kern_return_t configget(mach_port_t, xmlData,
    mach_msg_type_number_t, xmlDataOut, mach_msg_type_number_t *,
    int *, int *);

static mach_port_t selftest_port;

static void *
selftest_server(void *arg)
{
	(void)arg;
	configd_serve(selftest_port);
	return NULL;
}

static int
run_selftest(void)
{
	pthread_t		th;
	kern_return_t		kr;
	mach_port_t		session = MACH_PORT_NULL;
	int			status = -1;
	int			newInstance = 0;
	/* Non-const uint8_t arrays — xmlData is uint8_t[], and a cast
	 * from a string literal would trip -Wcast-qual under WARNS. */
	uint8_t			name[] = "selftest";
	uint8_t			empty[1] = { 0 };
	uint8_t			key[] = "selftest:key";
	uint8_t			val[] = "configd selftest value blob";
	xmlDataOut		got;
	mach_msg_type_number_t	gotCnt = 0;

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
	    &selftest_port);
	if (kr != KERN_SUCCESS) {
		clog("selftest: mach_port_allocate failed: 0x%x", (unsigned)kr);
		return 1;
	}
	kr = mach_port_insert_right(mach_task_self(), selftest_port,
	    selftest_port, MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		clog("selftest: insert_right failed: 0x%x", (unsigned)kr);
		return 1;
	}

	if (pthread_create(&th, NULL, selftest_server, NULL) != 0) {
		clog("selftest: pthread_create failed");
		return 1;
	}
	(void)sleep(1);		/* let the server thread reach mach_msg RCV */

	kr = configopen(selftest_port, name,
	    (mach_msg_type_number_t)(sizeof(name) - 1), empty, 0,
	    &session, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: configopen kr=0x%x status=%d", (unsigned)kr,
		    status);
		return 1;
	}

	kr = configset(session, key, (mach_msg_type_number_t)(sizeof(key) - 1),
	    val, (mach_msg_type_number_t)(sizeof(val) - 1), 0, &newInstance,
	    &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: configset kr=0x%x status=%d", (unsigned)kr,
		    status);
		return 1;
	}

	kr = configget(session, key, (mach_msg_type_number_t)(sizeof(key) - 1),
	    got, &gotCnt, &newInstance, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: configget kr=0x%x status=%d", (unsigned)kr,
		    status);
		return 1;
	}
	if (gotCnt != (mach_msg_type_number_t)(sizeof(val) - 1) ||
	    memcmp(got, val, gotCnt) != 0) {
		clog("selftest: value MISMATCH (gotCnt=%u)", (unsigned)gotCnt);
		return 1;
	}

	clog("CONFIGD-SELFTEST-OK: in-process config.defs round-trip works");
	return 0;
}

int
main(int argc, char **argv)
{
	mach_port_t	service_port = MACH_PORT_NULL;
	kern_return_t	kr;
	const char	*service_name = SCD_SERVER;

	(void)signal(SIGTERM, on_signal);
	(void)signal(SIGINT, on_signal);

	if (argc > 1 && strcmp(argv[1], "--selftest") == 0)
		return run_selftest();

	/*
	 * Check the SCDynamicStore service in with launchd. configd is a
	 * launchd job whose plist declares this MachService, so launchd
	 * created the port and hands us the receive right here.
	 */
	kr = bootstrap_check_in(bootstrap_port, service_name, &service_port);
	if (kr != KERN_SUCCESS) {	/* BOOTSTRAP_SUCCESS == KERN_SUCCESS == 0 */
		clog("bootstrap_check_in(%s) failed: 0x%x — exiting",
		    service_name, (unsigned)kr);
		return 1;
	}
	clog("Mach service '%s' checked in (port=0x%x)",
	    service_name, (unsigned)service_port);

	configd_serve(service_port);

	clog("shutting down (signal %d)", (int)got_term);
	return 0;
}
