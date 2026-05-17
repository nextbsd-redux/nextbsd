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

/* enum sandbox_filter_type — Apple casts to this. Define with same
 * values as the macros above. */
enum sandbox_filter_type {
	sandbox_filter_none			= SANDBOX_FILTER_NONE,
	sandbox_filter_path			= SANDBOX_FILTER_PATH,
	sandbox_filter_global_name		= SANDBOX_FILTER_GLOBAL_NAME,
	sandbox_filter_local_name		= SANDBOX_FILTER_LOCAL_NAME,
	sandbox_filter_appleevent_destination	= SANDBOX_FILTER_APPLEEVENT_DESTINATION,
	sandbox_filter_right_name		= SANDBOX_FILTER_RIGHT_NAME,
	sandbox_filter_notification		= SANDBOX_FILTER_NOTIFICATION,
	sandbox_filter_kext_bundle_id		= SANDBOX_FILTER_KEXT_BUNDLE_ID,
	sandbox_filter_info_type		= SANDBOX_FILTER_INFO_TYPE,
	sandbox_filter_notification_name	= SANDBOX_FILTER_NOTIFICATION_NAME,
	sandbox_filter_process_path		= SANDBOX_FILTER_PROCESS_PATH,
	sandbox_filter_entitlement		= SANDBOX_FILTER_ENTITLEMENT,
	sandbox_filter_preference_domain	= SANDBOX_FILTER_PREFERENCE_DOMAIN,
};

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
