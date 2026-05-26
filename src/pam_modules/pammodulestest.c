/*
 * pammodulestest — PAM port iter 2 (issue #95) CI gate.
 *
 * Dlopens each of the 5 vendored Apple standalone modules
 * (pam_self, pam_rootok, pam_uwtmp, pam_nologin, pam_env) from
 * /usr/lib/pam_NAME.so.6 and verifies the standard pam_sm_*
 * entry points exist. Emits PAM-MODULES-OK when all 5 pass;
 * PAM-MODULES-FAIL on the first failure.
 *
 * Why this and not pam_start(): iter 2 doesn't yet ship overlay
 * pam.d service files referencing these modules — that's iter 3.
 * Until then the modules sit in /usr/lib/ unreferenced by any PAM
 * service. The most surgical check is direct dlopen + dlsym to
 * prove the .so files are well-formed and ABI-compatible with our
 * libpam.so.6 (which iter 1 already verified holistically).
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct module_check {
	const char *path;
	const char *required_sym;	/* one canonical pam_sm_* entry */
};

/* Each module exposes at least one pam_sm_* entry that matches the
 * facility it implements. Checking one is enough to prove the .so
 * is a real PAM module and not a corrupted file. */
static const struct module_check checks[] = {
	{ "/usr/lib/pam_self.so.6",     "pam_sm_authenticate" },
	{ "/usr/lib/pam_rootok.so.6",   "pam_sm_authenticate" },
	{ "/usr/lib/pam_uwtmp.so.6",    "pam_sm_open_session" },
	{ "/usr/lib/pam_nologin.so.6",  "pam_sm_authenticate" },
	{ "/usr/lib/pam_env.so.6",      "pam_sm_open_session" },
};

int
main(void)
{
	size_t i;
	int failures = 0;

	for (i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
		void *h;
		void *sym;

		h = dlopen(checks[i].path, RTLD_NOW | RTLD_LOCAL);
		if (h == NULL) {
			(void)printf("PAM-MODULES-FAIL: dlopen %s: %s\n",
			    checks[i].path, dlerror());
			failures++;
			continue;
		}
		sym = dlsym(h, checks[i].required_sym);
		if (sym == NULL) {
			(void)printf("PAM-MODULES-FAIL: dlsym %s in %s: %s\n",
			    checks[i].required_sym, checks[i].path,
			    dlerror());
			(void)dlclose(h);
			failures++;
			continue;
		}
		(void)printf("  %s: dlopen ok, %s present\n",
		    checks[i].path, checks[i].required_sym);
		(void)dlclose(h);
	}

	if (failures > 0) {
		(void)printf("PAM-MODULES-FAIL: %d module(s) failed checks\n",
		    failures);
		return (1);
	}

	(void)printf("PAM-MODULES-OK: all 5 Apple modules loadable + ABI "
	    "valid (pam_self, pam_rootok, pam_uwtmp, pam_nologin, "
	    "pam_env via /usr/lib/pam_*.so.6)\n");
	return (0);
}
