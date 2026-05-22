/*
 * patterntest — configd regex pattern-watch test client (configd
 * iter 5).
 *
 * Like notifytest, but the watcher registers a POSIX regular
 * expression instead of an explicit key. session A watches the
 * pattern "patterntest:[0-9]+" and a notification port; session B
 * then changes two keys — one that matches the pattern and one that
 * does not. configd must notify session A for the matching key only:
 * the matching change must arrive (notification message +
 * notifychanges), and the non-matching change must not. Prints
 * CONFIGD-PATTERN-OK on success — the CI boot test (run.sh /
 * boot-test.sh) matches that marker.
 *
 * Speaks config.defs directly via the MIG user stub configUser.c, the
 * same way configtest and notifytest do.
 */

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"		/* MIG user stubs + xmlData; pulls config_types.h */
#include "config_session.h"	/* CONFIGD_NOTIFY_MSGID — the notify-msg contract */

#define SERVICE		"com.apple.SystemConfiguration.configd"

/* Open a configd session on `server`; returns the session port or NULL. */
static mach_port_t
open_session(mach_port_t server, const char *name)
{
	mach_port_t	session = MACH_PORT_NULL;
	int		status = -1;
	kern_return_t	kr;

	kr = configopen(server, (uint8_t *)name,
	    (mach_msg_type_number_t)strlen(name), (uint8_t *)"", 0,
	    &session, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		printf("CONFIGD-PATTERN-FAIL: configopen(%s) kr=0x%x "
		    "status=%d\n", name, (unsigned)kr, status);
		return MACH_PORT_NULL;
	}
	return session;
}

/*
 * change_list_has — walk the [uint32 little-endian length][key bytes]
 * records notifychanges returns; 1 if `key` is among them.
 */
static int
change_list_has(const uint8_t *list, size_t len, const char *key)
{
	size_t klen = strlen(key);
	size_t off = 0;

	while (off + sizeof(uint32_t) <= len) {
		uint32_t reclen;

		memcpy(&reclen, list + off, sizeof(reclen));
		off += sizeof(reclen);
		if (off + reclen > len)
			break;		/* truncated record */
		if (reclen == klen && memcmp(list + off, key, klen) == 0)
			return 1;
		off += reclen;
	}
	return 0;
}

/* Store `key`=`val` from session B. Returns 0 on success. */
static int
set_key(mach_port_t session, const char *key, const char *val)
{
	kern_return_t	kr;
	int		status = -1;
	int		newInstance = 0;

	kr = configset(session, (uint8_t *)key,
	    (mach_msg_type_number_t)strlen(key), (uint8_t *)val,
	    (mach_msg_type_number_t)strlen(val), 0, &newInstance, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		printf("CONFIGD-PATTERN-FAIL: configset(%s) kr=0x%x "
		    "status=%d\n", key, (unsigned)kr, status);
		return -1;
	}
	return 0;
}

int
main(void)
{
	mach_port_t		server = MACH_PORT_NULL;
	mach_port_t		sessionA = MACH_PORT_NULL;
	mach_port_t		sessionB = MACH_PORT_NULL;
	mach_port_t		notify_port = MACH_PORT_NULL;
	kern_return_t		kr;
	int			status = -1;
	const char		*pattern = "patterntest:[0-9]+";
	const char		*matchKey = "patterntest:42";
	const char		*missKey = "patterntest:nodigits";
	const char		*val = "configd iter 5 pattern value";
	xmlDataOut		list;
	mach_msg_type_number_t	listCnt = 0;
	union {
		mach_msg_header_t	hdr;
		uint8_t			buf[256];	/* header + trailer */
	} nmsg;

	kr = bootstrap_look_up(bootstrap_port, SERVICE, &server);
	if (kr != KERN_SUCCESS) {
		printf("CONFIGD-PATTERN-FAIL: bootstrap_look_up: 0x%x\n",
		    (unsigned)kr);
		return 1;
	}

	sessionA = open_session(server, "patterntest-watcher");
	if (sessionA == MACH_PORT_NULL)
		return 1;
	sessionB = open_session(server, "patterntest-writer");
	if (sessionB == MACH_PORT_NULL)
		return 1;

	/* session A watches the regex pattern. */
	kr = notifyadd(sessionA, (uint8_t *)pattern,
	    (mach_msg_type_number_t)strlen(pattern), 1, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		printf("CONFIGD-PATTERN-FAIL: notifyadd(regex) kr=0x%x "
		    "status=%d\n", (unsigned)kr, status);
		return 1;
	}

	/* A notification port: hand configd a send right, keep the
	 * receive right to listen on. */
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
	    &notify_port);
	if (kr != KERN_SUCCESS) {
		printf("CONFIGD-PATTERN-FAIL: mach_port_allocate: 0x%x\n",
		    (unsigned)kr);
		return 1;
	}
	kr = mach_port_insert_right(mach_task_self(), notify_port,
	    notify_port, MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("CONFIGD-PATTERN-FAIL: insert_right: 0x%x\n",
		    (unsigned)kr);
		return 1;
	}
	kr = notifyviaport(sessionA, notify_port, 0, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		printf("CONFIGD-PATTERN-FAIL: notifyviaport kr=0x%x "
		    "status=%d\n", (unsigned)kr, status);
		return 1;
	}

	/* session B changes a key that matches the pattern. */
	if (set_key(sessionB, matchKey, val) != 0)
		return 1;

	/* configd must have messaged session A's notification port. */
	memset(&nmsg, 0, sizeof(nmsg));
	kr = mach_msg(&nmsg.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
	    sizeof(nmsg), notify_port, 5000, MACH_PORT_NULL);
	if (kr != MACH_MSG_SUCCESS) {
		printf("CONFIGD-PATTERN-FAIL: no notification for matching "
		    "key (mach_msg: 0x%x)\n", (unsigned)kr);
		return 1;
	}

	/* notifychanges must report the matching key. */
	kr = notifychanges(sessionA, list, &listCnt, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		printf("CONFIGD-PATTERN-FAIL: notifychanges kr=0x%x "
		    "status=%d\n", (unsigned)kr, status);
		return 1;
	}
	if (!change_list_has(list, listCnt, matchKey)) {
		printf("CONFIGD-PATTERN-FAIL: changed-key list (%u bytes) "
		    "does not contain matching key '%s'\n",
		    (unsigned)listCnt, matchKey);
		return 1;
	}

	/* session B changes a key that does NOT match the pattern. */
	if (set_key(sessionB, missKey, val) != 0)
		return 1;

	/* configd must NOT message the notification port for it. */
	memset(&nmsg, 0, sizeof(nmsg));
	kr = mach_msg(&nmsg.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
	    sizeof(nmsg), notify_port, 500, MACH_PORT_NULL);
	if (kr != MACH_RCV_TIMED_OUT) {
		printf("CONFIGD-PATTERN-FAIL: unexpected notification for "
		    "non-matching key (mach_msg: 0x%x)\n", (unsigned)kr);
		return 1;
	}

	/* ...and notifychanges must report nothing pending. */
	listCnt = 0;
	kr = notifychanges(sessionA, list, &listCnt, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		printf("CONFIGD-PATTERN-FAIL: notifychanges(2) kr=0x%x "
		    "status=%d\n", (unsigned)kr, status);
		return 1;
	}
	if (listCnt != 0) {
		printf("CONFIGD-PATTERN-FAIL: non-matching key registered a "
		    "change (%u bytes pending)\n", (unsigned)listCnt);
		return 1;
	}

	printf("CONFIGD-PATTERN-OK: configd regex pattern watch round-trip "
	    "(matching key notified, non-matching key ignored)\n");
	return 0;
}
