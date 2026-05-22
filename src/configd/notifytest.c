/*
 * notifytest — configd change-notification round-trip test client
 * (configd iter 4).
 *
 * Opens two independent SCDynamicStore sessions with configd, which
 * exercises per-session Mach ports: session A is the watcher, session
 * B the writer. session A watches a key (notifyadd) and registers a
 * Mach notification port (notifyviaport); session B then changes that
 * key (configset). configd must message session A's notification port,
 * and notifychanges on session A must report the changed key. Prints
 * CONFIGD-NOTIFY-OK on success — the CI boot test (run.sh /
 * boot-test.sh) matches that marker.
 *
 * Speaks config.defs directly via the MIG user stub configUser.c, the
 * same way configtest exercises the store round-trip.
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
		printf("CONFIGD-NOTIFY-FAIL: configopen(%s) kr=0x%x status=%d\n",
		    name, (unsigned)kr, status);
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

int
main(void)
{
	mach_port_t		server = MACH_PORT_NULL;
	mach_port_t		sessionA = MACH_PORT_NULL;
	mach_port_t		sessionB = MACH_PORT_NULL;
	mach_port_t		notify_port = MACH_PORT_NULL;
	kern_return_t		kr;
	int			status = -1;
	int			newInstance = 0;
	const char		*key = "notifytest:watched";
	const char		*val = "configd iter 4 notification value";
	xmlDataOut		list;
	mach_msg_type_number_t	listCnt = 0;
	union {
		mach_msg_header_t	hdr;
		uint8_t			buf[256];	/* header + trailer */
	} nmsg;

	kr = bootstrap_look_up(bootstrap_port, SERVICE, &server);
	if (kr != KERN_SUCCESS) {
		printf("CONFIGD-NOTIFY-FAIL: bootstrap_look_up: 0x%x\n",
		    (unsigned)kr);
		return 1;
	}

	/* Two independent sessions — each gets its own per-session port. */
	sessionA = open_session(server, "notifytest-watcher");
	if (sessionA == MACH_PORT_NULL)
		return 1;
	sessionB = open_session(server, "notifytest-writer");
	if (sessionB == MACH_PORT_NULL)
		return 1;
	if (sessionA == sessionB) {
		printf("CONFIGD-NOTIFY-FAIL: both sessions share port 0x%x "
		    "(per-session ports not working)\n", (unsigned)sessionA);
		return 1;
	}

	/* session A watches the key. */
	kr = notifyadd(sessionA, (uint8_t *)key,
	    (mach_msg_type_number_t)strlen(key), 0, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		printf("CONFIGD-NOTIFY-FAIL: notifyadd kr=0x%x status=%d\n",
		    (unsigned)kr, status);
		return 1;
	}

	/*
	 * A notification port: allocate a receive right and hand configd
	 * a send right — notifyviaport's mach_port_move_send_t moves it
	 * in. notifytest keeps the receive right to listen on.
	 */
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
	    &notify_port);
	if (kr != KERN_SUCCESS) {
		printf("CONFIGD-NOTIFY-FAIL: mach_port_allocate: 0x%x\n",
		    (unsigned)kr);
		return 1;
	}
	kr = mach_port_insert_right(mach_task_self(), notify_port,
	    notify_port, MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("CONFIGD-NOTIFY-FAIL: insert_right: 0x%x\n",
		    (unsigned)kr);
		return 1;
	}
	kr = notifyviaport(sessionA, notify_port, 0, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		printf("CONFIGD-NOTIFY-FAIL: notifyviaport kr=0x%x status=%d\n",
		    (unsigned)kr, status);
		return 1;
	}

	/* session B changes the key session A is watching. */
	kr = configset(sessionB, (uint8_t *)key,
	    (mach_msg_type_number_t)strlen(key), (uint8_t *)val,
	    (mach_msg_type_number_t)strlen(val), 0, &newInstance, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		printf("CONFIGD-NOTIFY-FAIL: configset kr=0x%x status=%d\n",
		    (unsigned)kr, status);
		return 1;
	}

	/* configd must have messaged session A's notification port. */
	memset(&nmsg, 0, sizeof(nmsg));
	kr = mach_msg(&nmsg.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
	    sizeof(nmsg), notify_port, 5000, MACH_PORT_NULL);
	if (kr != MACH_MSG_SUCCESS) {
		printf("CONFIGD-NOTIFY-FAIL: no notification message "
		    "(mach_msg: 0x%x)\n", (unsigned)kr);
		return 1;
	}
	if (nmsg.hdr.msgh_id != CONFIGD_NOTIFY_MSGID) {
		printf("CONFIGD-NOTIFY-FAIL: notification msgh_id 0x%x "
		    "(expected 0x%x)\n", (unsigned)nmsg.hdr.msgh_id,
		    (unsigned)CONFIGD_NOTIFY_MSGID);
		return 1;
	}

	/* notifychanges on session A must report the changed key. */
	kr = notifychanges(sessionA, list, &listCnt, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		printf("CONFIGD-NOTIFY-FAIL: notifychanges kr=0x%x status=%d\n",
		    (unsigned)kr, status);
		return 1;
	}
	if (!change_list_has(list, listCnt, key)) {
		printf("CONFIGD-NOTIFY-FAIL: changed-key list (%u bytes) "
		    "does not contain '%s'\n", (unsigned)listCnt, key);
		return 1;
	}

	printf("CONFIGD-NOTIFY-OK: configd change notification round-trip "
	    "(2 sessions, watch + notify port + notifychanges)\n");
	return 0;
}
