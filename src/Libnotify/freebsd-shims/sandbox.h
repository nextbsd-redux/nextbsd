/* sandbox.h — FreeBSD shim. Apple's sandbox profile API; notifyd
 * applies a TrustedBSD sandbox profile at startup on macOS. FreeBSD
 * has Capsicum + MAC framework but no direct sandbox(3) equivalent.
 * Stub the calls to no-op success — daemon runs unsandboxed (matches
 * pre-Capsicum FreeBSD daemon norm). */
#ifndef _FREEBSD_SHIM_SANDBOX_H_
#define _FREEBSD_SHIM_SANDBOX_H_

#include <stdint.h>
#include <stddef.h>

#define SANDBOX_NAMED		0x0001
#define SANDBOX_NAMED_BUILTIN	0x0002

/* Apple sandbox_check filter types. */
#define SANDBOX_FILTER_NONE			0
#define SANDBOX_FILTER_PATH			1
#define SANDBOX_FILTER_GLOBAL_NAME		2
#define SANDBOX_FILTER_LOCAL_NAME		3
#define SANDBOX_FILTER_APPLEEVENT_DESTINATION	4
#define SANDBOX_FILTER_RIGHT_NAME		5
#define SANDBOX_FILTER_NOTIFICATION		6
#define SANDBOX_FILTER_KEXT_BUNDLE_ID		7
#define SANDBOX_FILTER_INFO_TYPE		8
#define SANDBOX_FILTER_NOTIFICATION_NAME	9
#define SANDBOX_FILTER_PROCESS_PATH		10
#define SANDBOX_FILTER_ENTITLEMENT		11
#define SANDBOX_FILTER_PREFERENCE_DOMAIN	12

static inline int
sandbox_init(const char *profile, uint64_t flags, char **errorbuf)
{
	(void)profile; (void)flags;
	if (errorbuf) *errorbuf = NULL;
	return 0;
}

static inline void
sandbox_free_error(char *errorbuf) { (void)errorbuf; }

static inline int
sandbox_check(int pid, const char *op, ...) { (void)pid; (void)op; return 0; }

#endif
