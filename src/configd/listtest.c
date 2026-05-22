/*
 * listtest — configd key-listing test client (configd iter 6).
 *
 * Opens a configd session, stores a handful of keys under a known
 * prefix plus one key outside it, then exercises configlist
 * (SCDynamicStoreCopyKeyList) three ways:
 *   - a prefix query ("listtest:")  -> the prefixed keys only;
 *   - an empty-key query            -> every key in the store;
 *   - a POSIX-regex query           -> the keys matching the pattern.
 * Each result must contain the keys it should and omit the ones it
 * should not. Prints CONFIGD-LIST-OK on success — the CI boot test
 * (run.sh / boot-test.sh) matches that marker.
 *
 * Speaks config.defs directly via the MIG user stub configUser.c, the
 * same way configtest / notifytest / patterntest do.
 */

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"		/* MIG user stubs + xmlData; pulls config_types.h */

#define SERVICE		"com.apple.SystemConfiguration.configd"

static const char *KEY_ALPHA	= "listtest:alpha";
static const char *KEY_BETA	= "listtest:beta";
static const char *KEY_GAMMA	= "listtest:gamma";
static const char *KEY_OUTSIDE	= "outsidelist:key";

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
		printf("CONFIGD-LIST-FAIL: configopen(%s) kr=0x%x status=%d\n",
		    name, (unsigned)kr, status);
		return MACH_PORT_NULL;
	}
	return session;
}

/* Store `key`=`val`. Returns 0 on success. */
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
		printf("CONFIGD-LIST-FAIL: configset(%s) kr=0x%x status=%d\n",
		    key, (unsigned)kr, status);
		return -1;
	}
	return 0;
}

/*
 * list_has — walk the [uint32 little-endian length][key bytes] records
 * configlist returns; 1 if `key` is among them.
 */
static int
list_has(const uint8_t *list, size_t len, const char *key)
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

/* Run a configlist query; returns 0 and fills list/listCnt on success. */
static int
list_keys(mach_port_t session, const char *key, int isRegex,
    uint8_t *list, mach_msg_type_number_t *listCnt)
{
	kern_return_t	kr;
	int		status = -1;

	*listCnt = 0;
	kr = configlist(session, (uint8_t *)key,
	    (mach_msg_type_number_t)strlen(key), isRegex, list, listCnt,
	    &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		printf("CONFIGD-LIST-FAIL: configlist(%s,isRegex=%d) kr=0x%x "
		    "status=%d\n", key, isRegex, (unsigned)kr, status);
		return -1;
	}
	return 0;
}

int
main(void)
{
	mach_port_t		server = MACH_PORT_NULL;
	mach_port_t		session = MACH_PORT_NULL;
	kern_return_t		kr;
	const char		*val = "configd iter 6 list value";
	xmlDataOut		list;
	mach_msg_type_number_t	listCnt = 0;

	kr = bootstrap_look_up(bootstrap_port, SERVICE, &server);
	if (kr != KERN_SUCCESS) {
		printf("CONFIGD-LIST-FAIL: bootstrap_look_up: 0x%x\n",
		    (unsigned)kr);
		return 1;
	}

	session = open_session(server, "listtest");
	if (session == MACH_PORT_NULL)
		return 1;

	/* Three keys under a known prefix, plus one outside it. */
	if (set_key(session, KEY_ALPHA, val) != 0 ||
	    set_key(session, KEY_BETA, val) != 0 ||
	    set_key(session, KEY_GAMMA, val) != 0 ||
	    set_key(session, KEY_OUTSIDE, val) != 0)
		return 1;

	/* Prefix query: the three "listtest:" keys, not the outside one. */
	if (list_keys(session, "listtest:", 0, list, &listCnt) != 0)
		return 1;
	if (!list_has(list, listCnt, KEY_ALPHA) ||
	    !list_has(list, listCnt, KEY_BETA) ||
	    !list_has(list, listCnt, KEY_GAMMA)) {
		printf("CONFIGD-LIST-FAIL: prefix query missing a "
		    "\"listtest:\" key\n");
		return 1;
	}
	if (list_has(list, listCnt, KEY_OUTSIDE)) {
		printf("CONFIGD-LIST-FAIL: prefix query returned the "
		    "non-prefixed key '%s'\n", KEY_OUTSIDE);
		return 1;
	}

	/* Empty-key query: every key, including the outside one. */
	if (list_keys(session, "", 0, list, &listCnt) != 0)
		return 1;
	if (!list_has(list, listCnt, KEY_ALPHA) ||
	    !list_has(list, listCnt, KEY_GAMMA) ||
	    !list_has(list, listCnt, KEY_OUTSIDE)) {
		printf("CONFIGD-LIST-FAIL: empty-key query missing a "
		    "stored key\n");
		return 1;
	}

	/* Regex query: only the keys matching the pattern. */
	if (list_keys(session, "listtest:(alpha|beta)", 1, list, &listCnt) != 0)
		return 1;
	if (!list_has(list, listCnt, KEY_ALPHA) ||
	    !list_has(list, listCnt, KEY_BETA)) {
		printf("CONFIGD-LIST-FAIL: regex query missing a "
		    "matching key\n");
		return 1;
	}
	if (list_has(list, listCnt, KEY_GAMMA) ||
	    list_has(list, listCnt, KEY_OUTSIDE)) {
		printf("CONFIGD-LIST-FAIL: regex query returned a "
		    "non-matching key\n");
		return 1;
	}

	printf("CONFIGD-LIST-OK: configd key listing round-trip "
	    "(prefix / all / regex queries)\n");
	return 0;
}
