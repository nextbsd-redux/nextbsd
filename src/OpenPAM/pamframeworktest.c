/*
 * pamframeworktest — PAM port iter 1 (issue #93) CI gate.
 *
 * Programmatic libpam round-trip: pam_start a session against the
 * fixture service /etc/pam.d/test_iter1 (which contains only
 * `auth required pam_deny.so`), call pam_authenticate, and emit
 * the PAM-FRAMEWORK-OK marker iff the framework returned the
 * expected PAM_AUTH_ERR — which proves three things at once:
 *
 *   1. Our libpam.so.6 (Apple OpenPAM-35) is what /usr/lib/libpam.so
 *      resolves to (test binary linked against -lpam).
 *   2. Our pam_deny.so loaded successfully via dlopen from
 *      /usr/lib/pam/ when libpam parsed the test_iter1 stack.
 *   3. The module ABI is intact end-to-end: pam_deny's
 *      pam_sm_authenticate fired and returned PAM_AUTH_ERR; libpam
 *      mapped that to pam_authenticate()'s return value.
 *
 * Anything else (segfault, no symbol, wrong return code, missing
 * service file) flips us to PAM-FRAMEWORK-FAIL.
 */

#include <security/pam_appl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Null conversation function — pam_deny.so doesn't prompt, so we
 * never actually get called. Defensive: if some future module
 * tried to prompt under this service name, returning PAM_CONV_ERR
 * would surface in the diagnostic rather than hang the test.
 */
static int
null_conv(int num_msg, const struct pam_message **msg,
    struct pam_response **resp, void *appdata)
{
	(void)num_msg;
	(void)msg;
	(void)resp;
	(void)appdata;
	return (PAM_CONV_ERR);
}

int
main(void)
{
	struct pam_conv conv = { null_conv, NULL };
	pam_handle_t *pamh = NULL;
	int rc;

	rc = pam_start("test_iter1", "root", &conv, &pamh);
	if (rc != PAM_SUCCESS || pamh == NULL) {
		(void)printf("PAM-FRAMEWORK-FAIL: pam_start "
		    "rc=%d (%s)\n", rc, pam_strerror(NULL, rc));
		return (1);
	}

	rc = pam_authenticate(pamh, 0);

	(void)pam_end(pamh, rc);

	if (rc == PAM_AUTH_ERR) {
		(void)printf("PAM-FRAMEWORK-OK: pam_deny.so loaded via "
		    "our libpam.so.6 and returned PAM_AUTH_ERR as "
		    "expected (full module ABI round-trip)\n");
		return (0);
	}

	(void)printf("PAM-FRAMEWORK-FAIL: expected PAM_AUTH_ERR (%d), "
	    "got rc=%d (%s)\n", PAM_AUTH_ERR, rc,
	    pam_strerror(NULL, rc));
	return (1);
}
