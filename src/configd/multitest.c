/*
 * multitest — configd batch-routine test client (configd iter 7).
 *
 * Exercises the three batch config.defs routines end to end against a
 * live configd:
 *   - configset_m : set two keys and remove two others in one call;
 *   - configget_m : fetch several values in one call, by explicit key
 *                   and by regex pattern;
 *   - notifyset   : replace the session's whole watch set (keys plus
 *                   patterns) in one call, then confirm a change to a
 *                   watched key / pattern is reported and a change to
 *                   an unwatched key is not.
 * Prints CONFIGD-MULTI-OK on success — the CI boot test (run.sh /
 * boot-test.sh) matches that marker.
 *
 * The batch payloads use the config_wire key-list / key-value
 * encodings (config_wire.c is linked in); the routines themselves come
 * from the MIG user stub configUser.c, as in the other test clients.
 */

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"		/* MIG user stubs + xmlData; pulls config_types.h */
#include "config_wire.h"	/* wire_keylist_* / wire_kvmap_* batch encodings */

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
		printf("CONFIGD-MULTI-FAIL: configopen(%s) kr=0x%x status=%d\n",
		    name, (unsigned)kr, status);
		return MACH_PORT_NULL;
	}
	return session;
}

/* Store `key`=`val` with a single configset. Returns 0 on success. */
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
		printf("CONFIGD-MULTI-FAIL: configset(%s) kr=0x%x status=%d\n",
		    key, (unsigned)kr, status);
		return -1;
	}
	return 0;
}

/* Single configget; returns the SCStatus, or -1 on a MIG failure. */
static int
get_status(mach_port_t session, const char *key)
{
	kern_return_t		kr;
	int			status = -1;
	int			newInstance = 0;
	xmlDataOut		data;
	mach_msg_type_number_t	dataCnt = 0;

	kr = configget(session, (uint8_t *)key,
	    (mach_msg_type_number_t)strlen(key), data, &dataCnt,
	    &newInstance, &status);
	if (kr != KERN_SUCCESS)
		return -1;
	return status;
}

/* 1 if the wire_kvmap payload holds `key` with value `val`. */
static int
kvmap_check(const uint8_t *buf, size_t len, const char *key, const char *val)
{
	const uint8_t	*cur = buf;
	const uint8_t	*end = buf + len;
	const void	*k, *v;
	size_t		kl, vl;
	size_t		klen = strlen(key);
	size_t		vlen = strlen(val);

	while (wire_kvmap_next(&cur, end, &k, &kl, &v, &vl)) {
		if (kl == klen && memcmp(k, key, klen) == 0)
			return vl == vlen && memcmp(v, val, vlen) == 0;
	}
	return 0;
}

/* 1 if the wire_keylist payload contains `key`. */
static int
keylist_check(const uint8_t *buf, size_t len, const char *key)
{
	const uint8_t	*cur = buf;
	const uint8_t	*end = buf + len;
	const void	*k;
	size_t		kl;
	size_t		klen = strlen(key);

	while (wire_keylist_next(&cur, end, &k, &kl)) {
		if (kl == klen && memcmp(k, key, klen) == 0)
			return 1;
	}
	return 0;
}

int
main(void)
{
	mach_port_t		server = MACH_PORT_NULL;
	mach_port_t		session = MACH_PORT_NULL;
	kern_return_t		kr;
	int			status = -1;
	uint8_t			req[1024];
	size_t			off;
	xmlDataOut		data;
	mach_msg_type_number_t	dataCnt = 0;

	kr = bootstrap_look_up(bootstrap_port, SERVICE, &server);
	if (kr != KERN_SUCCESS) {
		printf("CONFIGD-MULTI-FAIL: bootstrap_look_up: 0x%x\n",
		    (unsigned)kr);
		return 1;
	}
	session = open_session(server, "multitest");
	if (session == MACH_PORT_NULL)
		return 1;

	/* Two keys that configset_m will remove. */
	if (set_key(session, "multitest:old1", "x") != 0 ||
	    set_key(session, "multitest:old2", "x") != 0)
		return 1;

	/*
	 * configset_m: set multitest:a and multitest:b, remove the two
	 * "old" keys, in one call (notify list empty).
	 */
	off = 0;
	if (wire_kvmap_put(req, sizeof(req), &off, "multitest:a", 11,
	    "value-A", 7) != 0 ||
	    wire_kvmap_put(req, sizeof(req), &off, "multitest:b", 11,
	    "value-B", 7) != 0) {
		printf("CONFIGD-MULTI-FAIL: configset_m data build failed\n");
		return 1;
	}
	{
		uint8_t	rm[256];
		size_t	rmoff = 0;

		if (wire_keylist_put(rm, sizeof(rm), &rmoff, "multitest:old1",
		    14) != 0 ||
		    wire_keylist_put(rm, sizeof(rm), &rmoff, "multitest:old2",
		    14) != 0) {
			printf("CONFIGD-MULTI-FAIL: configset_m remove build "
			    "failed\n");
			return 1;
		}
		kr = configset_m(session, req, (mach_msg_type_number_t)off,
		    rm, (mach_msg_type_number_t)rmoff, (uint8_t *)"", 0,
		    &status);
	}
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		printf("CONFIGD-MULTI-FAIL: configset_m kr=0x%x status=%d\n",
		    (unsigned)kr, status);
		return 1;
	}

	/* The two set keys must exist; the two removed keys must not. */
	if (get_status(session, "multitest:a") != kSCStatusOK ||
	    get_status(session, "multitest:b") != kSCStatusOK) {
		printf("CONFIGD-MULTI-FAIL: configset_m did not set a key\n");
		return 1;
	}
	if (get_status(session, "multitest:old1") != kSCStatusNoKey ||
	    get_status(session, "multitest:old2") != kSCStatusNoKey) {
		printf("CONFIGD-MULTI-FAIL: configset_m did not remove a "
		    "key\n");
		return 1;
	}

	/*
	 * configget_m by explicit key: request multitest:a and
	 * multitest:b; the reply must carry both with their values.
	 */
	off = 0;
	if (wire_keylist_put(req, sizeof(req), &off, "multitest:a", 11) != 0 ||
	    wire_keylist_put(req, sizeof(req), &off, "multitest:b", 11) != 0) {
		printf("CONFIGD-MULTI-FAIL: configget_m keys build failed\n");
		return 1;
	}
	dataCnt = 0;
	kr = configget_m(session, req, (mach_msg_type_number_t)off,
	    (uint8_t *)"", 0, data, &dataCnt, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		printf("CONFIGD-MULTI-FAIL: configget_m(keys) kr=0x%x "
		    "status=%d\n", (unsigned)kr, status);
		return 1;
	}
	if (!kvmap_check(data, dataCnt, "multitest:a", "value-A") ||
	    !kvmap_check(data, dataCnt, "multitest:b", "value-B")) {
		printf("CONFIGD-MULTI-FAIL: configget_m(keys) reply missing "
		    "a key/value\n");
		return 1;
	}

	/*
	 * configget_m by pattern: a regex matching both keys must return
	 * both key/value pairs.
	 */
	off = 0;
	if (wire_keylist_put(req, sizeof(req), &off, "multitest:[ab]",
	    14) != 0) {
		printf("CONFIGD-MULTI-FAIL: configget_m pattern build "
		    "failed\n");
		return 1;
	}
	dataCnt = 0;
	kr = configget_m(session, (uint8_t *)"", 0, req,
	    (mach_msg_type_number_t)off, data, &dataCnt, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		printf("CONFIGD-MULTI-FAIL: configget_m(pattern) kr=0x%x "
		    "status=%d\n", (unsigned)kr, status);
		return 1;
	}
	if (!kvmap_check(data, dataCnt, "multitest:a", "value-A") ||
	    !kvmap_check(data, dataCnt, "multitest:b", "value-B")) {
		printf("CONFIGD-MULTI-FAIL: configget_m(pattern) reply "
		    "missing a key/value\n");
		return 1;
	}

	/*
	 * notifyset: replace the session's watch set with one explicit
	 * key and one pattern, then change a watched key, a
	 * pattern-matched key and an unwatched key.
	 */
	off = 0;
	if (wire_keylist_put(req, sizeof(req), &off, "multitest:a", 11) != 0) {
		printf("CONFIGD-MULTI-FAIL: notifyset keys build failed\n");
		return 1;
	}
	{
		uint8_t	pat[256];
		size_t	patoff = 0;

		if (wire_keylist_put(pat, sizeof(pat), &patoff,
		    "multitest:c.*", 13) != 0) {
			printf("CONFIGD-MULTI-FAIL: notifyset patterns build "
			    "failed\n");
			return 1;
		}
		kr = notifyset(session, req, (mach_msg_type_number_t)off,
		    pat, (mach_msg_type_number_t)patoff, &status);
	}
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		printf("CONFIGD-MULTI-FAIL: notifyset kr=0x%x status=%d\n",
		    (unsigned)kr, status);
		return 1;
	}

	if (set_key(session, "multitest:a", "value-A2") != 0 ||
	    set_key(session, "multitest:cZ", "value-C") != 0 ||
	    set_key(session, "multitest:nomatch", "value-N") != 0)
		return 1;

	dataCnt = 0;
	kr = notifychanges(session, data, &dataCnt, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		printf("CONFIGD-MULTI-FAIL: notifychanges kr=0x%x status=%d\n",
		    (unsigned)kr, status);
		return 1;
	}
	if (!keylist_check(data, dataCnt, "multitest:a") ||
	    !keylist_check(data, dataCnt, "multitest:cZ")) {
		printf("CONFIGD-MULTI-FAIL: notifyset watch missed a "
		    "changed key\n");
		return 1;
	}
	if (keylist_check(data, dataCnt, "multitest:nomatch")) {
		printf("CONFIGD-MULTI-FAIL: notifyset reported an unwatched "
		    "key\n");
		return 1;
	}

	printf("CONFIGD-MULTI-OK: configd batch routines round-trip "
	    "(configset_m / configget_m / notifyset)\n");
	return 0;
}
