/*
 * mach/mach_traps.h — public libmach API for the no-arg Mach trap family.
 *
 * Each function corresponds to a syscall registered by mach.ko at module
 * load via kern_syscall_register. The FreeBSD syscall number is dynamic —
 * libmach resolves it from sysctl mach.syscall.<name> on first call and
 * caches the result.
 *
 * If mach.ko is not loaded or the syscall isn't registered, these
 * functions return MACH_PORT_NULL.
 */
#ifndef _MACH_MACH_TRAPS_H_
#define _MACH_MACH_TRAPS_H_

#include <stdint.h>		/* uint32_t, uint64_t, int32_t */
#include <mach/kern_return.h>	/* kern_return_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int mach_port_name_t;

#define MACH_PORT_NULL ((mach_port_name_t)0)

mach_port_name_t mach_reply_port(void);
mach_port_name_t mach_task_self(void);
mach_port_name_t mach_thread_self(void);
mach_port_name_t mach_host_self(void);

/*
 * pid_for_task() — map a task port back to its owning pid. Apple-
 * canonical; lives alongside the other task-introspection traps.
 */
kern_return_t pid_for_task(mach_port_name_t task, int *pid);

/*
 * task_self_trap() — raw-syscall name for "give me my task port".
 * Some Apple source uses this rather than mach_task_self().
 */
mach_port_name_t task_self_trap(void);

/*
 * task_name_for_pid() — open a task-name port (introspection-only)
 * for a given pid. Stubbed; returns KERN_FAILURE.
 */
kern_return_t task_name_for_pid(mach_port_name_t target_task,
    int pid, mach_port_name_t *t);

/*
 * thread_switch() — Apple's Mach trap that voluntarily yields the
 * current thread, optionally directing the scheduler to run a
 * specific thread next. libdispatch's shims/yield.h + shims/lock.c
 * call it for adaptive spin/wait on contention paths.
 *
 * FreeBSD has no equivalent kernel trap; we back it with
 * sched_yield(). The thread argument is ignored, and the kernel
 * never gets to do directed handoff — calls just yield.
 *
 * `option` selects yield flavor (Apple enum). We honor
 * SWITCH_OPTION_WAIT by sleeping for `option_time` ms before
 * yielding; SWITCH_OPTION_NONE / SWITCH_OPTION_DEPRESS just yield.
 */
#ifndef _SWITCH_OPTION_DEFINED
#define _SWITCH_OPTION_DEFINED
#define SWITCH_OPTION_NONE	0
#define SWITCH_OPTION_DEPRESS	1
#define SWITCH_OPTION_WAIT	2
#endif

kern_return_t thread_switch(mach_port_name_t thread_name, int option,
    uint32_t option_time);

/*
 * thread_destruct_special_reply_port() — XNU-private trap that
 * releases the per-thread "special reply port" used by sync-IPC
 * mach_msg_send_sync. libdispatch's mach.c references it on the
 * DISPATCH_USE_MACH_SEND_SYNC_OVERRIDE branch (turned off on
 * FreeBSD; see internal.h:782 — the macro never gets defined to 1
 * on a non-macOS-10.13 target). The function is still
 * referenced in compiled-but-dead-at-runtime code, so we provide
 * a declaration and a stub returning KERN_FAILURE.
 */
enum thread_destruct_special_reply_port_rights {
	THREAD_SPECIAL_REPLY_PORT_ALL = 0,
	THREAD_SPECIAL_REPLY_PORT_RECEIVE_ONLY = 1,
	THREAD_SPECIAL_REPLY_PORT_SEND_ONLY = 2,
};

kern_return_t thread_destruct_special_reply_port(mach_port_name_t reply_port,
    enum thread_destruct_special_reply_port_rights rights);

/*
 * mach_port_destruct() — Apple's port-deallocation trap that drops
 * receive + send-once rights in a single call. libdispatch's mach.c
 * uses it for thread MIG reply ports. Routed to mach_port_deallocate
 * for now — the receive-vs-send-once distinction is a no-op for the
 * single-process port universe we have today.
 */
typedef int32_t mach_port_delta_t;
typedef uint64_t mach_port_context_t;

kern_return_t mach_port_destruct(mach_port_name_t task,
    mach_port_name_t name, mach_port_delta_t srdelta,
    mach_port_context_t guard);

/*
 * mach_port_construct — Apple's port creation with options (guards,
 * qlimit, etc.). libdispatch's notify-port-init path passes
 * MPO_CONTEXT_AS_GUARD | MPO_STRICT plus a per-port guard cookie.
 * Forward-decl `struct mach_port_options` here so we don't have to
 * pull in <mach/message.h>; the actual struct is defined there.
 */
struct mach_port_options;
typedef struct mach_port_options *mach_port_options_ptr_t_decl;
kern_return_t mach_port_construct(mach_port_name_t task,
    struct mach_port_options *opts, mach_port_context_t guard,
    mach_port_name_t *name);

/*
 * host_get_host_port — return the unprivileged host port. Apple's
 * mach_host_self() returns the HOST_PRIV port for root processes and
 * the unprivileged host port otherwise; libdispatch
 * (_dispatch_mach_host_port_init) tries to demote via this trap.
 * Stub: succeed with the input port (already unprivileged on our
 * stack).
 */
kern_return_t host_get_host_port(mach_port_name_t host_priv,
    mach_port_name_t *host_port);

/*
 * host_request_notification — Apple host-notification subscription
 * (calendar change, etc.). libdispatch registers a port to receive
 * HOST_NOTIFY_CALENDAR_CHANGE notifies. Stub: KERN_SUCCESS (no
 * notifications ever delivered). Calendar-change updates can be
 * picked up via clock_gettime/timezone polling if needed.
 */
kern_return_t host_request_notification(mach_port_name_t host,
    int notify_type, mach_port_name_t notify_port);

#ifdef __cplusplus
}
#endif

#endif /* !_MACH_MACH_TRAPS_H_ */
