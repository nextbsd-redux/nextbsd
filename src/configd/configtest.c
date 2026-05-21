/*
 * configtest — configd store round-trip test client (configd iter 3).
 *
 * Looks the com.apple.SystemConfiguration.configd Mach service up,
 * opens a session (configopen), then exercises the SCDynamicStore
 * over the MIG config.defs routines: stores a key (configset), reads
 * it back and checks the value round-trips byte for byte (configget),
 * removes it (configremove), and confirms a get of the removed key
 * now reports kSCStatusNoKey. Prints CONFIGD-STORE-OK on success —
 * the CI boot test (run.sh / boot-test.sh) matches that marker.
 *
 * This is a protocol-level client: it speaks config.defs directly
 * (built against the MIG user stub configUser.c), the same way
 * hwregquery speaks hwreg.defs. The CF-typed SystemConfiguration
 * framework — SCDynamicStoreCreate / SCDynamicStoreCopyValue / ... —
 * is a later iteration, for when a real consumer needs that API.
 */

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <stdio.h>
#include <string.h>

#include "config.h"		/* MIG user-side routine prototypes + types */

/* SCDynamicStore status codes (subset of SCError.h) — see configd.c. */
#define kSCStatusOK	0
#define kSCStatusNoKey	1004

#define SERVICE		"com.apple.SystemConfiguration.configd"

int
main(void)
{
	mach_port_t		server = MACH_PORT_NULL;
	mach_port_t		session = MACH_PORT_NULL;
	kern_return_t		kr;
	int			status = -1;
	int			newInstance = 0;
	const char		*key = "configtest:roundtrip";
	const char		*val = "configd iter 3 round-trip value";
	xmlDataOut_t		got = NULL;
	mach_msg_type_number_t	gotCnt = 0;

	kr = bootstrap_look_up(bootstrap_port, SERVICE, &server);
	if (kr != KERN_SUCCESS) {
		printf("CONFIGD-STORE-FAIL: bootstrap_look_up: 0x%x\n",
		    (unsigned)kr);
		return 1;
	}

	/* Open a store session. */
	kr = configopen(server, "configtest",
	    (mach_msg_type_number_t)strlen("configtest"), "", 0,
	    &session, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		printf("CONFIGD-STORE-FAIL: configopen kr=0x%x status=%d\n",
		    (unsigned)kr, status);
		return 1;
	}

	/* Store a key. */
	kr = configset(session, key, (mach_msg_type_number_t)strlen(key),
	    val, (mach_msg_type_number_t)strlen(val), 0, &newInstance,
	    &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		printf("CONFIGD-STORE-FAIL: configset kr=0x%x status=%d\n",
		    (unsigned)kr, status);
		return 1;
	}

	/* Read it back; the value must round-trip byte for byte. */
	kr = configget(session, key, (mach_msg_type_number_t)strlen(key),
	    &got, &gotCnt, &newInstance, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		printf("CONFIGD-STORE-FAIL: configget kr=0x%x status=%d\n",
		    (unsigned)kr, status);
		return 1;
	}
	if (gotCnt != (mach_msg_type_number_t)strlen(val) ||
	    memcmp(got, val, gotCnt) != 0) {
		printf("CONFIGD-STORE-FAIL: value mismatch "
		    "(got %u bytes, expected %zu)\n",
		    (unsigned)gotCnt, strlen(val));
		return 1;
	}

	/* Remove the key. */
	kr = configremove(session, key,
	    (mach_msg_type_number_t)strlen(key), &status);
	if (kr != KERN_SUCCESS || status != kSCStatusOK) {
		printf("CONFIGD-STORE-FAIL: configremove kr=0x%x status=%d\n",
		    (unsigned)kr, status);
		return 1;
	}

	/* A get of the removed key must now report "no such key". */
	got = NULL;
	gotCnt = 0;
	kr = configget(session, key, (mach_msg_type_number_t)strlen(key),
	    &got, &gotCnt, &newInstance, &status);
	if (kr != KERN_SUCCESS || status != kSCStatusNoKey) {
		printf("CONFIGD-STORE-FAIL: get-after-remove kr=0x%x "
		    "status=%d (expected kSCStatusNoKey %d)\n",
		    (unsigned)kr, status, kSCStatusNoKey);
		return 1;
	}

	printf("CONFIGD-STORE-OK: configd set / get / remove round-trip "
	    "(%zu-byte value)\n", strlen(val));
	return 0;
}
