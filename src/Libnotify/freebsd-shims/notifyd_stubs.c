/*
 * notifyd_stubs.c — link-time stubs for Apple-private symbols
 * notifyd references but we don't yet provide. Each returns a
 * reasonable failure value; daemon won't fully function until real
 * impls land in mach.ko / libxpc / libdispatch, but the binary
 * compiles + installs.
 *
 * Pattern matches launchd-842's forward_stubs.c.
 */
#include <bsm/libbsm.h>
#include <mach/mach.h>
#include <pthread.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/* audit_token_to_* — FreeBSD libbsm has audit_token_to_au32 but
 * not these individual extractors. Provide them by calling au32. */
pid_t audit_token_to_pid(audit_token_t atoken) __attribute__((weak));
pid_t
audit_token_to_pid(audit_token_t atoken)
{
	uid_t uid; gid_t gid; pid_t pid; au_asid_t asid;
	audit_token_to_au32(atoken, NULL, &uid, &gid, NULL, NULL, &pid, &asid, NULL);
	(void)uid; (void)gid;
	return pid;
}

uid_t audit_token_to_euid(audit_token_t atoken) __attribute__((weak));
uid_t
audit_token_to_euid(audit_token_t atoken)
{
	uid_t euid;
	audit_token_to_au32(atoken, NULL, &euid, NULL, NULL, NULL, NULL, NULL, NULL);
	return euid;
}

gid_t audit_token_to_egid(audit_token_t atoken) __attribute__((weak));
gid_t
audit_token_to_egid(audit_token_t atoken)
{
	gid_t egid;
	audit_token_to_au32(atoken, NULL, NULL, &egid, NULL, NULL, NULL, NULL, NULL);
	return egid;
}

/* sandbox stub — always return 0 (allowed). */
int
sandbox_check_by_audit_token(audit_token_t target, const char *op, int filter, ...)
{
	(void)target; (void)op; (void)filter;
	return 0;
}

/* pthread_setugid_np — Apple-private thread credential. FreeBSD
 * has no per-thread credentials; setuid would change the whole
 * process. notifyd uses this to drop privs for specific RPC
 * handlers; stub no-op (handler runs with daemon's normal creds). */
int
pthread_setugid_np(uid_t uid, gid_t gid)
{
	(void)uid; (void)gid;
	return 0;
}

/* Apple MIG special-reply-port helpers — used by sync MIG calls
 * for priority inheritance. Return MACH_PORT_NULL. */
mach_port_t
mig_get_special_reply_port(void)
{
	return MACH_PORT_NULL;
}

void
mig_dealloc_special_reply_port(mach_port_t port)
{
	(void)port;
}

/* current_task — Apple's per-thread task pointer. Same as
 * mach_task_self() for our purposes. */
mach_port_t
current_task(void)
{
	return mach_task_self();
}

/* mach_port_construct / mach_port_destruct — Apple's extended
 * port-creation API (with mach_port_options_t). For our runtime,
 * fall back to plain mach_port_allocate. */
struct mach_port_options;
kern_return_t
mach_port_construct(mach_port_t task, struct mach_port_options *opts,
                    mach_port_context_t context, mach_port_name_t *name)
{
	(void)opts; (void)context;
	return mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, name);
}

kern_return_t
mach_port_destruct(mach_port_t task, mach_port_name_t name,
                   mach_port_delta_t srdelta, mach_port_context_t context)
{
	(void)srdelta; (void)context;
	return mach_port_deallocate(task, name);
}

/* XPC event publisher stubs. */
void
xpc_event_publisher_set_error_handler(void *publisher, void *handler)
{
	(void)publisher; (void)handler;
}

void
xpc_event_publisher_set_throttling(void *publisher, uint64_t interval)
{
	(void)publisher; (void)interval;
}

/* OS_BUG_INTERNAL — a "soft bug" marker function from os/log. */
void
OS_BUG_INTERNAL(const char *fmt, ...)
{
	(void)fmt;
}
