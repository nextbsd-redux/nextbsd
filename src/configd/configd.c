/*
 * configd.c — SCDynamicStore configuration daemon (freebsd-launchd-mach
 * port).
 *
 * Apple's configd hosts the SCDynamicStore — a system-wide key->plist
 * store with change notifications — over the MIG `config` subsystem
 * (config.defs, base id 20000). This port keeps that real Mach IPC
 * design (see the configd-port memory / plan).
 *
 * configd checks the bootstrap service in with launchd and runs a raw
 * mach_msg receive loop that hands each message to the MIG-generated
 * config_server() demux. Apple's configd.tproj drives IPC through
 * libdispatch's dispatch_mach channel API; that API is unfinished in
 * this port, so configd uses a raw mach_msg loop instead — the same
 * proven pattern hwregd and launchd use here.
 *
 * iter 4 adds per-session ports and change notifications:
 *   - configopen allocates a fresh per-session Mach port (config_session.c),
 *     hands the client a send right, and arms a no-senders notification
 *     so the session is torn down when the client exits. Every session
 *     port joins a port set, so the one receive loop serves them all;
 *     the port a request arrives on (the MIG `server` argument) names
 *     the session.
 *   - notifyadd / notifyviaport / notifychanges let a session watch
 *     explicit keys or POSIX-regex patterns (iter 5), register a
 *     notification port, and drain the keys that changed. configset /
 *     configremove / confignotify fan a change out to every watching
 *     session.
 *   - configlist (iter 6) lists store keys by prefix or POSIX regex.
 *   - notifyset / configget_m / configset_m (iter 7) do batch watch
 *     registration, multi-key fetch and multi-key update.
 * notifyviafd and snapshot remain stubs (see their handlers).
 *
 * config.defs carries the key/value payloads INLINE in bounded arrays
 * — out-of-line Mach data is broken cross-process in this port's
 * kernel (see config.defs) — so a handler's xmlData argument is the
 * byte buffer itself; there is no out-of-line memory to free.
 *
 * `configd --selftest` runs an in-process round trip (a server thread
 * plus the client stubs) — no launchd or bootstrap — as a quick
 * self-check of the config.defs RPC and notification path.
 */

#include <sys/types.h>

#include <mach/mach.h>
#include <mach/notify.h>		/* MACH_NOTIFY_NO_SENDERS */
#include <servers/bootstrap.h>

#include <pthread.h>
#include <regex.h>			/* regcomp / regexec — configlist isRegex */
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config_types.h"		/* SCD_SERVER, kSCStatus*, CONFIG_DATA_MAX */
#include "config_store.h"
#include "config_session.h"
#include "config_wire.h"	/* keylist / kvmap batch encodings */
#include "configServer.h"	/* MIG: config_server() demux + _config* protos */

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
 * kvmap_has — 1 if the wire_kvmap-encoded buffer `buf` (of `len`
 * bytes) already holds a record for key/klen. configget_m uses it to
 * keep its reply a set: a key matched by several patterns, or both
 * requested and matched, is emitted once.
 */
static int
kvmap_has(const uint8_t *buf, size_t len, const void *key, size_t klen)
{
	const uint8_t	*cur = buf;
	const uint8_t	*end = buf + len;
	const void	*k;
	const void	*v;
	size_t		kl;
	size_t		vl;

	while (wire_kvmap_next(&cur, end, &k, &kl, &v, &vl)) {
		if (kl == klen && memcmp(k, key, klen) == 0)
			return 1;
	}
	return 0;
}

/*
 * MIG routine handlers — config_server() dispatches to these. An
 * xmlData / xmlDataOut argument is an inline byte buffer (config.defs
 * carries the payloads inline); the matching mach_msg_type_number_t
 * carries its length. Out-parameters are given a safe value first so
 * the MIG reply marshalling never ships an uninitialised port or
 * count. The add / multi variants, the regex paths and snapshot are
 * still stubs returning kSCStatusFailed.
 */

kern_return_t
_configopen(mach_port_t server, xmlData name, mach_msg_type_number_t nameCnt,
    xmlData options, mach_msg_type_number_t optionsCnt,
    mach_port_t *session, int *status)
{
	/*
	 * Hand the client its own per-session port. session_open()
	 * allocates the receive right, inserts a MAKE_SEND right (MIG's
	 * mach_port_move_send_t moves it out in the reply), joins it to
	 * configd's port set and arms the no-senders notification.
	 */
	(void)server;
	(void)name;
	(void)nameCnt;
	(void)options;
	(void)optionsCnt;

	if (session_open(session) != 0) {
		*session = MACH_PORT_NULL;
		*status = kSCStatusFailed;
		return KERN_SUCCESS;
	}
	*status = kSCStatusOK;
	return KERN_SUCCESS;
}

/*
 * configlist (SCDynamicStoreCopyKeyList) — return the store keys that
 * match `key`: with isRegex set, the keys matching `key` as a POSIX
 * regex; otherwise the keys prefixed by `key` (an empty `key` lists
 * every key). The result is the keylist_append [len][bytes] encoding.
 */
kern_return_t
_configlist(mach_port_t server, xmlData key, mach_msg_type_number_t keyCnt,
    int isRegex, xmlDataOut list, mach_msg_type_number_t *listCnt,
    int *status)
{
	size_t	count, i, off = 0;
	regex_t	re;
	int	have_re = 0;

	(void)server;
	*listCnt = 0;

	if (isRegex != 0) {
		char buf[CONFIG_DATA_MAX + 3];	/* ^ + pattern + $ + NUL */

		if (config_pattern_anchor(key, keyCnt, buf, sizeof(buf)) != 0 ||
		    regcomp(&re, buf, REG_EXTENDED) != 0) {
			*status = kSCStatusInvalidArgument;
			return KERN_SUCCESS;
		}
		have_re = 1;
	}

	count = store_count();
	for (i = 0; i < count; i++) {
		const void	*skey;
		size_t		sklen;
		int		match;

		if (store_key_at(i, &skey, &sklen) != 0)
			continue;

		if (isRegex != 0) {
			char keystr[CONFIG_DATA_MAX + 1];	/* NUL-term */

			if (sklen >= sizeof(keystr))
				continue;	/* key too long to match */
			memcpy(keystr, skey, sklen);
			keystr[sklen] = '\0';
			match = (regexec(&re, keystr, 0, NULL, 0) == 0);
		} else {
			/* prefix match; an empty key matches every key */
			match = (keyCnt == 0) ||
			    (sklen >= keyCnt &&
			     memcmp(skey, key, keyCnt) == 0);
		}

		if (match && wire_keylist_put(list, CONFIG_DATA_MAX, &off,
		    skey, sklen) != 0) {
			/* the packed list overflowed the inline reply */
			if (have_re)
				regfree(&re);
			*listCnt = 0;
			*status = kSCStatusFailed;
			return KERN_SUCCESS;
		}
	}

	if (have_re)
		regfree(&re);
	*listCnt = (mach_msg_type_number_t)off;
	*status = kSCStatusOK;
	return KERN_SUCCESS;
}

/*
 * configadd (SCDynamicStoreAddValue) — store a key/value, but only if
 * the key does not already exist. An existing key is left untouched
 * and reported as kSCStatusKeyExists; this is what distinguishes add
 * from set. Existence check via store_get, like _configget; the store
 * + change-notification path is _configset's.
 */
kern_return_t
_configadd(mach_port_t server, xmlData key, mach_msg_type_number_t keyCnt,
    xmlData data, mach_msg_type_number_t dataCnt, int *newInstance,
    int *status)
{
	const void	*val;
	size_t		vlen;

	(void)server;
	*newInstance = 0;

	if (keyCnt == 0) {
		*status = kSCStatusInvalidArgument;
	} else if (store_get(key, keyCnt, &val, &vlen) == 0) {
		/* key already defined — add must not clobber it */
		*status = kSCStatusKeyExists;
	} else if (store_set(key, keyCnt, data, dataCnt) != 0) {
		*status = kSCStatusFailed;
	} else {
		/* a new key is a change watching sessions must hear about */
		session_key_changed(key, keyCnt);
		*status = kSCStatusOK;
	}
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

	if (keyCnt == 0) {
		*status = kSCStatusInvalidArgument;
	} else if (store_set(key, keyCnt, data, dataCnt) != 0) {
		*status = kSCStatusFailed;
	} else {
		/* fan the change out to every watching session */
		session_key_changed(key, keyCnt);
		*status = kSCStatusOK;
	}
	return KERN_SUCCESS;
}

kern_return_t
_configremove(mach_port_t server, xmlData key, mach_msg_type_number_t keyCnt,
    int *status)
{
	(void)server;

	if (keyCnt == 0) {
		*status = kSCStatusInvalidArgument;
	} else if (store_remove(key, keyCnt) != 0) {
		*status = kSCStatusNoKey;
	} else {
		/* removing a key is a change watchers must hear about */
		session_key_changed(key, keyCnt);
		*status = kSCStatusOK;
	}
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

/*
 * confignotify (SCDynamicStoreNotifyValue) — force a change
 * notification for a key without touching its value.
 */
kern_return_t
_confignotify(mach_port_t server, xmlData key, mach_msg_type_number_t keyCnt,
    int *status)
{
	(void)server;

	if (keyCnt == 0) {
		*status = kSCStatusInvalidArgument;
	} else {
		session_key_changed(key, keyCnt);
		*status = kSCStatusOK;
	}
	return KERN_SUCCESS;
}

/*
 * configget_m (SCDynamicStoreCopyMultiple) — fetch several store
 * values at once: the values for the requested `keys` that exist,
 * plus the value of every store key matching one of the `patterns`.
 * keys and patterns are wire_keylist payloads; the reply `data` is a
 * wire_kvmap payload. Each key is emitted at most once.
 */
kern_return_t
_configget_m(mach_port_t server, xmlData keys, mach_msg_type_number_t keysCnt,
    xmlData patterns, mach_msg_type_number_t patternsCnt, xmlDataOut data,
    mach_msg_type_number_t *dataCnt, int *status)
{
	const uint8_t	*cur, *end;
	const void	*item;
	size_t		ilen, off = 0;

	(void)server;
	*dataCnt = 0;

	/* Requested explicit keys: emit the value of each that exists. */
	cur = keys;
	end = keys + keysCnt;
	while (wire_keylist_next(&cur, end, &item, &ilen)) {
		const void	*val;
		size_t		vlen;

		if (store_get(item, ilen, &val, &vlen) != 0)
			continue;	/* no such key — skip it */
		if (kvmap_has(data, off, item, ilen))
			continue;	/* already emitted */
		if (wire_kvmap_put(data, CONFIG_DATA_MAX, &off, item, ilen,
		    val, vlen) != 0) {
			*dataCnt = 0;
			*status = kSCStatusFailed;	/* reply overflowed */
			return KERN_SUCCESS;
		}
	}

	/* Pattern keys: emit the value of every store key that matches. */
	cur = patterns;
	end = patterns + patternsCnt;
	while (wire_keylist_next(&cur, end, &item, &ilen)) {
		char	abuf[CONFIG_DATA_MAX + 3];	/* ^ + pattern + $ + NUL */
		regex_t	re;
		size_t	i, count;

		if (config_pattern_anchor(item, ilen, abuf, sizeof(abuf)) != 0 ||
		    regcomp(&re, abuf, REG_EXTENDED) != 0) {
			*dataCnt = 0;
			*status = kSCStatusInvalidArgument;
			return KERN_SUCCESS;
		}

		count = store_count();
		for (i = 0; i < count; i++) {
			const void	*skey, *val;
			size_t		sklen, vlen;
			char		keystr[CONFIG_DATA_MAX + 1];

			if (store_key_at(i, &skey, &sklen) != 0)
				continue;
			if (sklen >= sizeof(keystr))
				continue;	/* too long to match */
			memcpy(keystr, skey, sklen);
			keystr[sklen] = '\0';
			if (regexec(&re, keystr, 0, NULL, 0) != 0)
				continue;	/* no match */
			if (kvmap_has(data, off, skey, sklen))
				continue;	/* already emitted */
			if (store_get(skey, sklen, &val, &vlen) != 0)
				continue;
			if (wire_kvmap_put(data, CONFIG_DATA_MAX, &off, skey,
			    sklen, val, vlen) != 0) {
				regfree(&re);
				*dataCnt = 0;
				*status = kSCStatusFailed;
				return KERN_SUCCESS;
			}
		}
		regfree(&re);
	}

	*dataCnt = (mach_msg_type_number_t)off;
	*status = kSCStatusOK;
	return KERN_SUCCESS;
}

/*
 * configset_m (SCDynamicStoreSetMultiple) — apply a batch of store
 * changes in one call: set every key/value pair in `data` (a
 * wire_kvmap payload), remove every key in `removeData`, and force a
 * change notification for every key in `notify` (both wire_keylist
 * payloads). Each set / remove / notify fans out to watching sessions.
 */
kern_return_t
_configset_m(mach_port_t server, xmlData data, mach_msg_type_number_t dataCnt,
    xmlData removeData, mach_msg_type_number_t removeCnt,
    xmlData notify, mach_msg_type_number_t notifyCnt, int *status)
{
	const uint8_t	*cur, *end;
	const void	*k, *v;
	size_t		kl, vl;

	(void)server;

	/* Set each key/value pair. */
	cur = data;
	end = data + dataCnt;
	while (wire_kvmap_next(&cur, end, &k, &kl, &v, &vl)) {
		if (kl == 0)
			continue;	/* skip an empty key */
		if (store_set(k, kl, v, vl) != 0) {
			*status = kSCStatusFailed;
			return KERN_SUCCESS;
		}
		session_key_changed(k, kl);
	}

	/* Remove each key, notifying watchers of the ones that existed. */
	cur = removeData;
	end = removeData + removeCnt;
	while (wire_keylist_next(&cur, end, &k, &kl)) {
		if (kl != 0 && store_remove(k, kl) == 0)
			session_key_changed(k, kl);
	}

	/* Force a change notification for each notify key. */
	cur = notify;
	end = notify + notifyCnt;
	while (wire_keylist_next(&cur, end, &k, &kl)) {
		if (kl != 0)
			session_key_changed(k, kl);
	}

	*status = kSCStatusOK;
	return KERN_SUCCESS;
}

/*
 * notifyadd (SCDynamicStoreAddWatchedKey) — add a watch to the session.
 * isRegex selects an explicit-key watch or a POSIX-regex pattern watch.
 */
kern_return_t
_notifyadd(mach_port_t server, xmlData key, mach_msg_type_number_t keyCnt,
    int isRegex, int *status)
{
	if (keyCnt == 0)
		*status = kSCStatusInvalidArgument;
	else if (isRegex != 0)
		*status = session_pattern_add(server, key, keyCnt);
	else
		*status = session_watch_add(server, key, keyCnt);
	return KERN_SUCCESS;
}

/*
 * notifyremove (SCDynamicStoreRemoveWatchedKey) — drop a watch from the
 * session; isRegex picks the explicit-key or the pattern watch list.
 */
kern_return_t
_notifyremove(mach_port_t server, xmlData key, mach_msg_type_number_t keyCnt,
    int isRegex, int *status)
{
	if (keyCnt == 0)
		*status = kSCStatusInvalidArgument;
	else if (isRegex != 0)
		*status = session_pattern_remove(server, key, keyCnt);
	else
		*status = session_watch_remove(server, key, keyCnt);
	return KERN_SUCCESS;
}

/*
 * notifychanges (SCDynamicStoreCopyNotifiedKeys) — return and clear the
 * session's list of keys that changed since the last call. The list is
 * encoded as a run of [uint32 little-endian length][key bytes] records.
 */
kern_return_t
_notifychanges(mach_port_t server, xmlDataOut list,
    mach_msg_type_number_t *listCnt, int *status)
{
	ssize_t n;

	*listCnt = 0;

	if (!session_valid(server)) {
		*status = kSCStatusNoStoreSession;
		return KERN_SUCCESS;
	}

	n = session_drain_changes(server, list, CONFIG_DATA_MAX);
	if (n < 0) {
		/* the encoded change list overflowed the inline reply */
		*status = kSCStatusFailed;
		return KERN_SUCCESS;
	}
	*listCnt = (mach_msg_type_number_t)n;
	*status = kSCStatusOK;
	return KERN_SUCCESS;
}

/*
 * notifyviaport (SCDynamicStoreNotifyMachPort) — register the Mach
 * port configd messages when a watched key changes. `port` arrives as
 * a moved-in send right configd takes ownership of.
 */
kern_return_t
_notifyviaport(mach_port_t server, mach_port_t port, mach_msg_id_t msgid,
    int *status)
{
	if (msgid != 0) {
		/* Apple's per-message identifier is obsolete; reject it. */
		if (port != MACH_PORT_NULL)
			(void)mach_port_deallocate(mach_task_self(), port);
		*status = kSCStatusInvalidArgument;
		return KERN_SUCCESS;
	}
	*status = session_set_notify_port(server, port);
	return KERN_SUCCESS;
}

/*
 * notifycancel (SCDynamicStoreNotifyCancel) — drop the session's
 * notification port and watch list.
 */
kern_return_t
_notifycancel(mach_port_t server, int *status)
{
	*status = session_clear_notify_port(server);
	return KERN_SUCCESS;
}

/*
 * notifyset (SCDynamicStoreSetNotificationKeys) — replace the
 * session's entire notification key set: after this call it watches
 * exactly the explicit `keys` and the regex `patterns` given (both
 * wire_keylist payloads).
 */
kern_return_t
_notifyset(mach_port_t server, xmlData keys, mach_msg_type_number_t keysCnt,
    xmlData patterns, mach_msg_type_number_t patternsCnt, int *status)
{
	const uint8_t	*cur, *end;
	const void	*item;
	size_t		ilen;
	int		rc;

	if (!session_valid(server)) {
		*status = kSCStatusNoStoreSession;
		return KERN_SUCCESS;
	}

	/* Drop the old watch set, then install the new one. */
	(void)session_watch_clear(server);

	cur = keys;
	end = keys + keysCnt;
	while (wire_keylist_next(&cur, end, &item, &ilen)) {
		rc = session_watch_add(server, item, ilen);
		if (rc != kSCStatusOK) {
			*status = rc;
			return KERN_SUCCESS;
		}
	}

	cur = patterns;
	end = patterns + patternsCnt;
	while (wire_keylist_next(&cur, end, &item, &ilen)) {
		rc = session_pattern_add(server, item, ilen);
		if (rc != kSCStatusOK) {
			*status = rc;
			return KERN_SUCCESS;
		}
	}

	*status = kSCStatusOK;
	return KERN_SUCCESS;
}

/*
 * notifyviafd (SCDynamicStoreNotifyFileDescriptor) — stubbed. It would
 * deliver notifications by writing to a client file descriptor passed
 * as a Mach fileport, but this port has no fileport->fd support; the
 * Mach-port path (notifyviaport) is the supported notification route.
 */
kern_return_t
_notifyviafd(mach_port_t server, mach_port_t fileport, int identifier,
    int *status)
{
	(void)server; (void)identifier;
	if (fileport != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), fileport);
	*status = kSCStatusFailed;
	return KERN_SUCCESS;
}

/*
 * snapshot — stubbed. Apple's configd writes a debug dump of the whole
 * store and session table to a file; it is a root-gated diagnostic
 * with no consumer in this port.
 */
kern_return_t
_snapshot(mach_port_t server, int *status)
{
	(void)server;
	*status = kSCStatusFailed;
	return KERN_SUCCESS;
}

/*
 * configd_serve — the raw mach_msg receive loop. It receives on a
 * port set holding the bootstrap service port and every per-session
 * port. config_server() is the MIG demux for the config subsystem; it
 * writes the routine's reply (or a MIG error) into `rep`. A
 * MACH_NOTIFY_NO_SENDERS message means a client exited — its session
 * port's no-senders notification fired — so the session is closed.
 * The 1s receive timeout lets the loop notice a SIGTERM for a clean
 * shutdown.
 */
static void
configd_serve(mach_port_t port_set)
{
	while (!got_term) {
		union {
			mach_msg_header_t hdr;
			char buf[CONFIGD_MSG_BUFSZ];
		} req, rep;
		mach_msg_return_t mr;

		memset(&req, 0, sizeof(req));
		mr = mach_msg(&req.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
		    sizeof(req), port_set, 1000, MACH_PORT_NULL);
		if (mr == MACH_RCV_TIMED_OUT)
			continue;
		if (mr != MACH_MSG_SUCCESS) {
			clog("mach_msg receive failed: 0x%x — exiting",
			    (unsigned)mr);
			break;
		}

		/* A client exited: tear its session down. */
		if (req.hdr.msgh_id == MACH_NOTIFY_NO_SENDERS) {
			if (session_close(req.hdr.msgh_local_port) == 0)
				clog("session port 0x%x closed (client gone)",
				    (unsigned)req.hdr.msgh_local_port);
			continue;
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
 * configd_make_port_set — allocate the port set configd_serve()
 * receives on and move `initial` (the bootstrap service port, or the
 * selftest port) into it. The session layer adds new session ports to
 * the same set. Returns 0 / -1.
 */
static int
configd_make_port_set(mach_port_t initial, mach_port_t *port_set)
{
	kern_return_t kr;

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET,
	    port_set);
	if (kr != KERN_SUCCESS) {
		clog("port set allocate failed: 0x%x", (unsigned)kr);
		return -1;
	}
	kr = mach_port_move_member(mach_task_self(), initial, *port_set);
	if (kr != KERN_SUCCESS) {
		clog("port set move_member failed: 0x%x", (unsigned)kr);
		return -1;
	}
	session_layer_init(*port_set);
	return 0;
}

/*
 * In-process self test (`configd --selftest`). A server thread runs
 * configd_serve() on a private port set; the main thread drives the
 * config.defs client stubs against it — configopen / configset /
 * configget, then notifyadd / configset / notifychanges — with no
 * launchd or bootstrap in the loop. The client stubs come from the
 * MIG-generated configUser.c.
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
extern kern_return_t configlist(mach_port_t, xmlData,
    mach_msg_type_number_t, int, xmlDataOut, mach_msg_type_number_t *,
    int *);
extern kern_return_t configget_m(mach_port_t, xmlData,
    mach_msg_type_number_t, xmlData, mach_msg_type_number_t, xmlDataOut,
    mach_msg_type_number_t *, int *);
extern kern_return_t configset_m(mach_port_t, xmlData,
    mach_msg_type_number_t, xmlData, mach_msg_type_number_t, xmlData,
    mach_msg_type_number_t, int *);
extern kern_return_t notifyadd(mach_port_t, xmlData,
    mach_msg_type_number_t, int, int *);
extern kern_return_t notifyviaport(mach_port_t, mach_port_t,
    mach_msg_id_t, int *);
extern kern_return_t notifychanges(mach_port_t, xmlDataOut,
    mach_msg_type_number_t *, int *);

/*
 * selftest_list_has — 1 if the keylist_append-encoded [len][bytes]
 * list `list` (of `len` bytes) contains a record equal to want/wlen.
 */
static int
selftest_list_has(const uint8_t *list, mach_msg_type_number_t len,
    const void *want, size_t wlen)
{
	size_t off = 0;

	while (off + sizeof(uint32_t) <= (size_t)len) {
		uint32_t reclen;

		memcpy(&reclen, list + off, sizeof(reclen));
		off += sizeof(reclen);
		if (off + reclen > (size_t)len)
			break;
		if (reclen == wlen && memcmp(list + off, want, wlen) == 0)
			return 1;
		off += reclen;
	}
	return 0;
}

static mach_port_t selftest_pset;

static void *
selftest_server(void *arg)
{
	(void)arg;
	configd_serve(selftest_pset);
	return NULL;
}

static int
run_selftest(void)
{
	pthread_t		th;
	kern_return_t		kr;
	mach_port_t		selftest_port = MACH_PORT_NULL;
	mach_port_t		session = MACH_PORT_NULL;
	mach_port_t		notify_port = MACH_PORT_NULL;
	int			status = -1;
	int			newInstance = 0;
	/* Non-const uint8_t arrays — xmlData is uint8_t[], and a cast
	 * from a string literal would trip -Wcast-qual under WARNS. */
	uint8_t			name[] = "selftest";
	uint8_t			empty[1] = { 0 };
	uint8_t			key[] = "selftest:key";
	uint8_t			pat[] = "selftest:pat[0-9]+";
	uint8_t			patkey[] = "selftest:pat42";
	uint8_t			val[] = "configd selftest value blob";
	uint8_t			val2[] = "configd selftest changed value";
	xmlDataOut		got;
	mach_msg_type_number_t	gotCnt = 0;
	union {
		mach_msg_header_t	hdr;
		uint8_t			buf[256];	/* header + trailer */
	} nmsg;

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
	if (configd_make_port_set(selftest_port, &selftest_pset) != 0)
		return 1;

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

	/*
	 * Change-notification path: watch the key, change it, and
	 * confirm notifychanges reports exactly that key.
	 */
	kr = notifyadd(session, key, (mach_msg_type_number_t)(sizeof(key) - 1),
	    0, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: notifyadd kr=0x%x status=%d", (unsigned)kr,
		    status);
		return 1;
	}

	kr = configset(session, key, (mach_msg_type_number_t)(sizeof(key) - 1),
	    val2, (mach_msg_type_number_t)(sizeof(val2) - 1), 0, &newInstance,
	    &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: configset(notify) kr=0x%x status=%d",
		    (unsigned)kr, status);
		return 1;
	}

	gotCnt = 0;
	kr = notifychanges(session, got, &gotCnt, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: notifychanges kr=0x%x status=%d", (unsigned)kr,
		    status);
		return 1;
	}
	{
		/* The list is one [uint32 len][key bytes] record. */
		uint32_t	klen;
		size_t		want = sizeof(key) - 1;

		if (gotCnt != (mach_msg_type_number_t)(sizeof(klen) + want)) {
			clog("selftest: notifychanges list size %u (want %zu)",
			    (unsigned)gotCnt, sizeof(klen) + want);
			return 1;
		}
		memcpy(&klen, got, sizeof(klen));
		if (klen != want ||
		    memcmp(got + sizeof(klen), key, want) != 0) {
			clog("selftest: notifychanges key MISMATCH");
			return 1;
		}
	}

	/*
	 * Notification-port path: register a Mach notification port,
	 * change the watched key again, and confirm configd messages
	 * the port.
	 */
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
	    &notify_port);
	if (kr != KERN_SUCCESS) {
		clog("selftest: notify-port allocate failed: 0x%x",
		    (unsigned)kr);
		return 1;
	}
	kr = mach_port_insert_right(mach_task_self(), notify_port,
	    notify_port, MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		clog("selftest: notify-port insert_right failed: 0x%x",
		    (unsigned)kr);
		return 1;
	}
	kr = notifyviaport(session, notify_port, 0, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: notifyviaport kr=0x%x status=%d", (unsigned)kr,
		    status);
		return 1;
	}

	kr = configset(session, key, (mach_msg_type_number_t)(sizeof(key) - 1),
	    val, (mach_msg_type_number_t)(sizeof(val) - 1), 0, &newInstance,
	    &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: configset(viaport) kr=0x%x status=%d",
		    (unsigned)kr, status);
		return 1;
	}

	memset(&nmsg, 0, sizeof(nmsg));
	kr = mach_msg(&nmsg.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
	    sizeof(nmsg), notify_port, 5000, MACH_PORT_NULL);
	if (kr != MACH_MSG_SUCCESS) {
		clog("selftest: no notification message (mach_msg: 0x%x)",
		    (unsigned)kr);
		return 1;
	}
	if (nmsg.hdr.msgh_id != CONFIGD_NOTIFY_MSGID) {
		clog("selftest: notification msgh_id 0x%x (expected 0x%x)",
		    (unsigned)nmsg.hdr.msgh_id, (unsigned)CONFIGD_NOTIFY_MSGID);
		return 1;
	}

	/* Drain the change left pending by the notify-port step. */
	gotCnt = 0;
	kr = notifychanges(session, got, &gotCnt, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: notifychanges(drain) kr=0x%x status=%d",
		    (unsigned)kr, status);
		return 1;
	}

	/*
	 * Pattern watch: register a POSIX regex, change a key that
	 * matches it, and confirm notifychanges reports that key.
	 */
	kr = notifyadd(session, pat, (mach_msg_type_number_t)(sizeof(pat) - 1),
	    1, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: notifyadd(regex) kr=0x%x status=%d",
		    (unsigned)kr, status);
		return 1;
	}
	kr = configset(session, patkey,
	    (mach_msg_type_number_t)(sizeof(patkey) - 1), val,
	    (mach_msg_type_number_t)(sizeof(val) - 1), 0, &newInstance,
	    &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: configset(regex) kr=0x%x status=%d",
		    (unsigned)kr, status);
		return 1;
	}
	gotCnt = 0;
	kr = notifychanges(session, got, &gotCnt, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: notifychanges(regex) kr=0x%x status=%d",
		    (unsigned)kr, status);
		return 1;
	}
	{
		/* One [uint32 len][key bytes] record — the matched key. */
		uint32_t	klen2;
		size_t		want2 = sizeof(patkey) - 1;

		if (gotCnt != (mach_msg_type_number_t)(sizeof(klen2) + want2)) {
			clog("selftest: regex change list size %u (want %zu)",
			    (unsigned)gotCnt, sizeof(klen2) + want2);
			return 1;
		}
		memcpy(&klen2, got, sizeof(klen2));
		if (klen2 != want2 ||
		    memcmp(got + sizeof(klen2), patkey, want2) != 0) {
			clog("selftest: regex change key MISMATCH");
			return 1;
		}
	}

	/*
	 * configlist: list every store key and confirm both keys this
	 * selftest stored (the explicit key and the pattern-matched key)
	 * appear in the result.
	 */
	gotCnt = 0;
	kr = configlist(session, empty, 0, 0, got, &gotCnt, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		clog("selftest: configlist kr=0x%x status=%d", (unsigned)kr,
		    status);
		return 1;
	}
	if (!selftest_list_has(got, gotCnt, key, sizeof(key) - 1) ||
	    !selftest_list_has(got, gotCnt, patkey, sizeof(patkey) - 1)) {
		clog("selftest: configlist is missing a stored key");
		return 1;
	}

	/*
	 * configset_m / configget_m: set a key through a batch update,
	 * fetch it back through a batch query, and confirm the value
	 * round-trips through the wire_kvmap encoding.
	 */
	{
		uint8_t		mkey[] = "selftest:multi";
		uint8_t		mval[] = "configd selftest multi value";
		uint8_t		reqbuf[256];
		size_t		reqoff;
		const uint8_t	*cur, *end;
		const void	*gk, *gv;
		size_t		gkl, gvl;
		int		found = 0;

		reqoff = 0;
		if (wire_kvmap_put(reqbuf, sizeof(reqbuf), &reqoff, mkey,
		    sizeof(mkey) - 1, mval, sizeof(mval) - 1) != 0) {
			clog("selftest: configset_m payload build failed");
			return 1;
		}
		kr = configset_m(session, reqbuf, (mach_msg_type_number_t)reqoff,
		    empty, 0, empty, 0, &status);
		if (kr != KERN_SUCCESS || status != kSCStatusOK) {
			clog("selftest: configset_m kr=0x%x status=%d",
			    (unsigned)kr, status);
			return 1;
		}

		reqoff = 0;
		if (wire_keylist_put(reqbuf, sizeof(reqbuf), &reqoff, mkey,
		    sizeof(mkey) - 1) != 0) {
			clog("selftest: configget_m payload build failed");
			return 1;
		}
		gotCnt = 0;
		kr = configget_m(session, reqbuf,
		    (mach_msg_type_number_t)reqoff, empty, 0, got, &gotCnt,
		    &status);
		if (kr != KERN_SUCCESS || status != kSCStatusOK) {
			clog("selftest: configget_m kr=0x%x status=%d",
			    (unsigned)kr, status);
			return 1;
		}

		cur = got;
		end = got + gotCnt;
		while (wire_kvmap_next(&cur, end, &gk, &gkl, &gv, &gvl)) {
			if (gkl != sizeof(mkey) - 1 ||
			    memcmp(gk, mkey, gkl) != 0)
				continue;
			found = 1;
			if (gvl != sizeof(mval) - 1 ||
			    memcmp(gv, mval, gvl) != 0) {
				clog("selftest: configget_m value MISMATCH");
				return 1;
			}
		}
		if (!found) {
			clog("selftest: configget_m did not return the key");
			return 1;
		}
	}

	clog("CONFIGD-SELFTEST-OK: in-process config.defs round-trip + "
	    "change notification + notify port + regex watch + list + "
	    "multi works");
	return 0;
}

int
main(int argc, char **argv)
{
	mach_port_t	service_port = MACH_PORT_NULL;
	mach_port_t	port_set = MACH_PORT_NULL;
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

	/*
	 * configd serves the bootstrap service port and every
	 * per-session port from one receive loop: a port set holds them
	 * all, and configopen joins new session ports to it.
	 */
	if (configd_make_port_set(service_port, &port_set) != 0) {
		clog("could not set up the configd port set — exiting");
		return 1;
	}

	configd_serve(port_set);

	clog("shutting down (signal %d)", (int)got_term);
	return 0;
}
