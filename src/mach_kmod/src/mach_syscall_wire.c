/*
 * mach_syscall_wire.c — wire individual Mach syscalls to the FreeBSD
 * sysent table at module load.
 *
 * Why this exists. ravynOS's mach_module.c uses syscall_helper_register
 * on an array of SYSCALL_INIT_HELPER entries. Our compat shim sets
 * every SYS_<name> = NO_SYSCALL because we don't have Apple's
 * syscalls.master to assign real numbers. kern_syscall_helper_register
 * uses NO_SYSCALL as the end-of-array sentinel, so its loop exits at
 * the first entry without registering anything. Result: handlers are
 * loaded in the kmod but unreachable from userland.
 *
 * This file does it the right way for an out-of-tree module: call
 * syscall_register directly with offset = NO_SYSCALL to trigger
 * dynamic slot allocation. The kernel picks an unused syscall number;
 * we expose it via `sysctl mach.syscall.<name>` so userland test code
 * can find it.
 *
 * Phase B / Tier 1 B scope: wire the no-arg "trap" family
 * (mach_reply_port, task_self_trap, …). Each entry adds a per-syscall
 * sysent + offset + sysctl. Same NULL-guard wrapper pattern: if the
 * caller has no Mach state (p_machdata == NULL), return MACH_PORT_NULL
 * instead of NULL-faulting in current_task().
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/sysent.h>
#include <sys/proc.h>

#include <sys/sysproto.h>		/* canonical PADL_/PADR_ macros — */
					/* MUST come before _mach_sysproto */
#include <bsm/audit_kevents.h>		/* AUE_NULL */
#include <sys/mach/_mach_sysproto.h>	/* mach_msg_trap_args (full def) */

/*
 * Forward-declare per-syscall arg structs. Full definitions come from
 * <sys/mach/_mach_sysproto.h> via mach_types.h's force-include in
 * compat_shim.h. We only need the prototype shape here because we
 * just take pointers to the sys_* handlers; the args structs aren't
 * dereferenced from this file.
 */
struct mach_reply_port_args;
struct task_self_trap_args;
struct thread_self_trap_args;
struct host_self_trap_args;
struct mach_msg_trap_args;
struct _kernelrpc_mach_port_allocate_trap_args;
struct _kernelrpc_mach_port_deallocate_trap_args;
struct _kernelrpc_mach_port_insert_right_trap_args;
struct task_get_special_port_trap_args;
struct task_set_special_port_trap_args;
struct host_set_special_port_trap_args;
struct _kernelrpc_mach_port_move_member_trap_args;
struct register_event_bell_trap_args;
struct unregister_event_bell_trap_args;
struct mach_wait_quiet_args;

SYSCTL_DECL(_mach);
static SYSCTL_NODE(_mach, OID_AUTO, syscall, CTLFLAG_RW, 0,
    "Mach syscall numbers (dynamically allocated)");

/*
 * Handler defined in src/mach_traps.c. Signature:
 *   int sys_mach_reply_port(struct thread *, struct mach_reply_port_args *)
 *
 * The Mach handler reads td->td_retval[0] = mach_reply_port() and
 * returns 0. mach_reply_port() internally calls ipc_port_alloc against
 * current_task()->itk_space, which requires the calling process to
 * have a Mach task (p_machdata) set up via mach_task_init's
 * process_init eventhandler. Processes forked AFTER mach.ko loaded
 * satisfy this; processes that existed before will fault.
 */
int sys_mach_reply_port(struct thread *, struct mach_reply_port_args *);
int sys_task_self_trap(struct thread *, struct task_self_trap_args *);
int sys_thread_self_trap(struct thread *, struct thread_self_trap_args *);
int sys_host_self_trap(struct thread *, struct host_self_trap_args *);
int sys_mach_msg_trap(struct thread *, struct mach_msg_trap_args *);
int sys__kernelrpc_mach_port_allocate_trap(struct thread *,
    struct _kernelrpc_mach_port_allocate_trap_args *);
int sys__kernelrpc_mach_port_deallocate_trap(struct thread *,
    struct _kernelrpc_mach_port_deallocate_trap_args *);
int sys__kernelrpc_mach_port_insert_right_trap(struct thread *,
    struct _kernelrpc_mach_port_insert_right_trap_args *);
int sys_task_get_special_port_trap(struct thread *,
    struct task_get_special_port_trap_args *);
int sys_task_set_special_port_trap(struct thread *,
    struct task_set_special_port_trap_args *);
int sys_host_set_special_port_trap(struct thread *,
    struct host_set_special_port_trap_args *);
int sys__kernelrpc_mach_port_move_member_trap(struct thread *,
    struct _kernelrpc_mach_port_move_member_trap_args *);
int sys_register_event_bell_trap(struct thread *,
    struct register_event_bell_trap_args *);
int sys_unregister_event_bell_trap(struct thread *,
    struct unregister_event_bell_trap_args *);
int sys_mach_wait_quiet(struct thread *, struct mach_wait_quiet_args *);

/*
 * Phase C2: lazy Mach init. If the calling process/thread has no
 * Mach state (it pre-existed mach.ko or the relevant eventhandlers
 * aren't running), allocate one now so the underlying Mach handler
 * has a real task/thread to operate on. Defined in src/kern/task.c
 * and src/mach_thread.c respectively.
 */
int mach_task_init_lazy(struct proc *p);
int mach_thread_init_lazy(struct thread *td);

/*
 * NULL-guard wrappers. The "no Mach state" path returns MACH_PORT_NULL
 * (0) — the same value Apple's API would return on no-resources —
 * instead of NULL-faulting in current_task() / itk_space. A future
 * Phase C lazy-init helper can promote the caller to a real
 * Mach-aware state.
 */
static int
sys_mach_reply_port_guarded(struct thread *td, struct mach_reply_port_args *uap)
{
	if (td->td_proc->p_machdata == NULL)
		mach_task_init_lazy(td->td_proc);
	if (td->td_proc->p_machdata == NULL) {
		td->td_retval[0] = 0;	/* MACH_PORT_NULL */
		return (0);
	}
	return (sys_mach_reply_port(td, uap));
}

static int
sys_task_self_trap_guarded(struct thread *td, struct task_self_trap_args *uap)
{
	if (td->td_proc->p_machdata == NULL)
		mach_task_init_lazy(td->td_proc);
	if (td->td_proc->p_machdata == NULL) {
		td->td_retval[0] = 0;	/* MACH_PORT_NULL */
		return (0);
	}
	return (sys_task_self_trap(td, uap));
}

static int
sys_thread_self_trap_guarded(struct thread *td, struct thread_self_trap_args *uap)
{
	if (td->td_proc->p_machdata == NULL)
		mach_task_init_lazy(td->td_proc);
	if (td->td_machdata == NULL)
		mach_thread_init_lazy(td);
	if (td->td_proc->p_machdata == NULL || td->td_machdata == NULL) {
		td->td_retval[0] = 0;	/* MACH_PORT_NULL */
		return (0);
	}
	return (sys_thread_self_trap(td, uap));
}

static int
sys_host_self_trap_guarded(struct thread *td, struct host_self_trap_args *uap)
{
	if (td->td_proc->p_machdata == NULL)
		mach_task_init_lazy(td->td_proc);
	if (td->td_machdata == NULL)
		mach_thread_init_lazy(td);
	if (td->td_proc->p_machdata == NULL || td->td_machdata == NULL) {
		td->td_retval[0] = 0;	/* MACH_PORT_NULL */
		return (0);
	}
	return (sys_host_self_trap(td, uap));
}

/*
 * mach_msg_trap takes real args (7 of them). The handler in
 * src/mach_traps.c forwards to sys_mach_msg_overwrite_trap with
 * rcv_msg=NULL. Both task and thread Mach state are required —
 * mach_msg internals reach current_task()->itk_space (send path)
 * and current_thread()->ith_* (receive path).
 */
static int
sys_mach_msg_trap_guarded(struct thread *td, struct mach_msg_trap_args *uap)
{
	if (td->td_proc->p_machdata == NULL)
		mach_task_init_lazy(td->td_proc);
	if (td->td_machdata == NULL)
		mach_thread_init_lazy(td);
	if (td->td_proc->p_machdata == NULL || td->td_machdata == NULL) {
		/* Mach state unavailable — return a generic error to userland.
		 * MACH_SEND_NO_BUFFER is the closest "we couldn't even try"
		 * code in the standard return set. */
		td->td_retval[0] = 0x1000000d;	/* MACH_SEND_NO_BUFFER */
		return (0);
	}
	uap->notify = 0;
	return (sys_mach_msg_trap(td, uap));
}

/*
 * Port-management traps used by libxpc (xpc_connection_create_mach_service
 * paths) and any consumer that needs to allocate Mach ports beyond the
 * task/thread/host/reply trio. All three reach current_task()->itk_space,
 * so they need Mach task state; thread state is not required.
 *
 * On no-Mach-state, return KERN_RESOURCE_SHORTAGE (0x6 from <sys/mach/
 * kern_return.h>) — the closest "couldn't allocate" code in the standard
 * return set. Userland sees a non-zero kr and handles it as a generic
 * resource failure rather than crashing.
 */
static int
sys__kernelrpc_mach_port_allocate_trap_guarded(struct thread *td,
    struct _kernelrpc_mach_port_allocate_trap_args *uap)
{
	if (td->td_proc->p_machdata == NULL)
		mach_task_init_lazy(td->td_proc);
	if (td->td_proc->p_machdata == NULL) {
		td->td_retval[0] = 6;	/* KERN_RESOURCE_SHORTAGE */
		return (0);
	}
	return (sys__kernelrpc_mach_port_allocate_trap(td, uap));
}

static int
sys__kernelrpc_mach_port_deallocate_trap_guarded(struct thread *td,
    struct _kernelrpc_mach_port_deallocate_trap_args *uap)
{
	if (td->td_proc->p_machdata == NULL)
		mach_task_init_lazy(td->td_proc);
	if (td->td_proc->p_machdata == NULL) {
		td->td_retval[0] = 6;	/* KERN_RESOURCE_SHORTAGE */
		return (0);
	}
	return (sys__kernelrpc_mach_port_deallocate_trap(td, uap));
}

static int
sys__kernelrpc_mach_port_insert_right_trap_guarded(struct thread *td,
    struct _kernelrpc_mach_port_insert_right_trap_args *uap)
{
	if (td->td_proc->p_machdata == NULL)
		mach_task_init_lazy(td->td_proc);
	if (td->td_proc->p_machdata == NULL) {
		td->td_retval[0] = 6;	/* KERN_RESOURCE_SHORTAGE */
		return (0);
	}
	return (sys__kernelrpc_mach_port_insert_right_trap(td, uap));
}

/*
 * Task special-port traps. Both reach current_task() and the task's
 * itk_<slot> fields, so they need Mach task state. KERN_INVALID_ARGUMENT
 * (4) is the closest "task is null" code in the standard return set
 * — same value task_get/set_special_port itself returns on TASK_NULL.
 */
static int
sys_task_get_special_port_trap_guarded(struct thread *td,
    struct task_get_special_port_trap_args *uap)
{
	if (td->td_proc->p_machdata == NULL)
		mach_task_init_lazy(td->td_proc);
	if (td->td_proc->p_machdata == NULL) {
		td->td_retval[0] = 4;	/* KERN_INVALID_ARGUMENT */
		return (0);
	}
	return (sys_task_get_special_port_trap(td, uap));
}

static int
sys_task_set_special_port_trap_guarded(struct thread *td,
    struct task_set_special_port_trap_args *uap)
{
	if (td->td_proc->p_machdata == NULL)
		mach_task_init_lazy(td->td_proc);
	if (td->td_proc->p_machdata == NULL) {
		td->td_retval[0] = 4;	/* KERN_INVALID_ARGUMENT */
		return (0);
	}
	return (sys_task_set_special_port_trap(td, uap));
}

static int
sys_host_set_special_port_trap_guarded(struct thread *td,
    struct host_set_special_port_trap_args *uap)
{
	if (td->td_proc->p_machdata == NULL)
		mach_task_init_lazy(td->td_proc);
	if (td->td_proc->p_machdata == NULL) {
		td->td_retval[0] = 4;	/* KERN_INVALID_ARGUMENT */
		return (0);
	}
	return (sys_host_set_special_port_trap(td, uap));
}

/*
 * mach_port_move_member — needs Mach task state; the handler reaches
 * current_task()->itk_space. On no-Mach-state return KERN_INVALID_ARGUMENT
 * (4), the closest "task is null" code in the standard return set.
 */
static int
sys__kernelrpc_mach_port_move_member_trap_guarded(struct thread *td,
    struct _kernelrpc_mach_port_move_member_trap_args *uap)
{
	if (td->td_proc->p_machdata == NULL)
		mach_task_init_lazy(td->td_proc);
	if (td->td_proc->p_machdata == NULL) {
		td->td_retval[0] = 4;	/* KERN_INVALID_ARGUMENT */
		return (0);
	}
	return (sys__kernelrpc_mach_port_move_member_trap(td, uap));
}

/*
 * Event-bell traps (Task #39 Path B). The bridge handlers reach
 * current_task() to resolve the pset name, so they need Mach task
 * state. On no-Mach-state return ENOSYS — matches the libmach
 * wrapper's "bell unavailable" expectation.
 */
static int
sys_register_event_bell_trap_guarded(struct thread *td,
    struct register_event_bell_trap_args *uap)
{
	if (td->td_proc->p_machdata == NULL)
		mach_task_init_lazy(td->td_proc);
	if (td->td_proc->p_machdata == NULL) {
		td->td_retval[0] = ENOSYS;
		return (0);
	}
	return (sys_register_event_bell_trap(td, uap));
}

static int
sys_unregister_event_bell_trap_guarded(struct thread *td,
    struct unregister_event_bell_trap_args *uap)
{
	if (td->td_proc->p_machdata == NULL)
		mach_task_init_lazy(td->td_proc);
	if (td->td_proc->p_machdata == NULL) {
		td->td_retval[0] = ENOSYS;
		return (0);
	}
	return (sys_unregister_event_bell_trap(td, uap));
}

/*
 * mach_wait_quiet — block until the device tree quiesces (mach.bus.busy
 * == 0). Unlike the Mach traps above, the handler reaches no Mach
 * task/thread state (it only sleeps on the busystate counter), so no
 * lazy-init / NULL-guard is needed; the wrapper is a thin pass-through
 * kept for symmetry with the other wired syscalls.
 */
static int
sys_mach_wait_quiet_guarded(struct thread *td, struct mach_wait_quiet_args *uap)
{
	return (sys_mach_wait_quiet(td, uap));
}

static struct sysent mach_reply_port_sysent = {
	.sy_narg	= 0,
	.sy_call	= (sy_call_t *)sys_mach_reply_port_guarded,
	.sy_auevent	= AUE_NULL,
	.sy_flags	= 0,
};

static struct sysent task_self_trap_sysent = {
	.sy_narg	= 0,
	.sy_call	= (sy_call_t *)sys_task_self_trap_guarded,
	.sy_auevent	= AUE_NULL,
	.sy_flags	= 0,
};

static struct sysent thread_self_trap_sysent = {
	.sy_narg	= 0,
	.sy_call	= (sy_call_t *)sys_thread_self_trap_guarded,
	.sy_auevent	= AUE_NULL,
	.sy_flags	= 0,
};

static struct sysent host_self_trap_sysent = {
	.sy_narg	= 0,
	.sy_call	= (sy_call_t *)sys_host_self_trap_guarded,
	.sy_auevent	= AUE_NULL,
	.sy_flags	= 0,
};

/*
 * mach_msg_trap as a 6-arg syscall — the trailing `notify` arg is
 * dropped because FreeBSD's libc syscall() only correctly passes 6
 * args via the kernel ABI on amd64. Our wrapper hard-codes notify
 * to MACH_PORT_NULL; libmach's mach_msg() accepts notify in its API
 * but ignores it. (Apple's mach_msg2_trap addresses this differently
 * by passing a packed argument descriptor; we can move to that model
 * later if real callers need notify.)
 */
static struct sysent mach_msg_trap_sysent = {
	.sy_narg	= 6,
	.sy_call	= (sy_call_t *)sys_mach_msg_trap_guarded,
	.sy_auevent	= AUE_NULL,
	.sy_flags	= 0,
};

/*
 * Port-management trap sysents. Argument counts come from the
 * _kernelrpc_*_trap_args struct definitions in <sys/mach/_mach_sysproto.h>:
 *   allocate:      (task, right, name*)             = 3
 *   deallocate:    (task, name)                     = 2
 *   insert_right:  (task, name, poly, polyPoly)     = 4
 * All fit comfortably under libc syscall()'s 6-arg ABI limit, no
 * 7th-arg workaround required (unlike mach_msg_trap).
 */
static struct sysent _kernelrpc_mach_port_allocate_sysent = {
	.sy_narg	= 3,
	.sy_call	= (sy_call_t *)sys__kernelrpc_mach_port_allocate_trap_guarded,
	.sy_auevent	= AUE_NULL,
	.sy_flags	= 0,
};

static struct sysent _kernelrpc_mach_port_deallocate_sysent = {
	.sy_narg	= 2,
	.sy_call	= (sy_call_t *)sys__kernelrpc_mach_port_deallocate_trap_guarded,
	.sy_auevent	= AUE_NULL,
	.sy_flags	= 0,
};

static struct sysent _kernelrpc_mach_port_insert_right_sysent = {
	.sy_narg	= 4,
	.sy_call	= (sy_call_t *)sys__kernelrpc_mach_port_insert_right_trap_guarded,
	.sy_auevent	= AUE_NULL,
	.sy_flags	= 0,
};

/*
 * Task special-port sysents. 3 args each:
 *   get: (target, which, port*)
 *   set: (target, which, port)
 */
static struct sysent task_get_special_port_sysent = {
	.sy_narg	= 3,
	.sy_call	= (sy_call_t *)sys_task_get_special_port_trap_guarded,
	.sy_auevent	= AUE_NULL,
	.sy_flags	= 0,
};

static struct sysent task_set_special_port_sysent = {
	.sy_narg	= 3,
	.sy_call	= (sy_call_t *)sys_task_set_special_port_trap_guarded,
	.sy_auevent	= AUE_NULL,
	.sy_flags	= 0,
};

/*
 * host_set_special_port sysent. 3 args: (host, which, port). Each Mach
 * trap now gets its own dedicated FreeBSD syscall slot (the kernel's
 * lkmnosys band is wide enough), auto-allocated via NO_SYSCALL.
 */
static struct sysent host_set_special_port_sysent = {
	.sy_narg	= 3,
	.sy_call	= (sy_call_t *)sys_host_set_special_port_trap_guarded,
	.sy_auevent	= AUE_NULL,
	.sy_flags	= 0,
};

/*
 * mach_port_move_member sysent. 3 args: (target, member, after). Wired
 * to the existing Apple-imported _kernelrpc trap handler.
 */
static struct sysent _kernelrpc_mach_port_move_member_sysent = {
	.sy_narg	= 3,
	.sy_call	= (sy_call_t *)sys__kernelrpc_mach_port_move_member_trap_guarded,
	.sy_auevent	= AUE_NULL,
	.sy_flags	= 0,
};

/*
 * Event-bell sysents (Task #39 Path B):
 *   register_event_bell:   (port, write_fd)  = 2
 *   unregister_event_bell: (port)            = 1
 */
static struct sysent register_event_bell_sysent = {
	.sy_narg	= 2,
	.sy_call	= (sy_call_t *)sys_register_event_bell_trap_guarded,
	.sy_auevent	= AUE_NULL,
	.sy_flags	= 0,
};

static struct sysent unregister_event_bell_sysent = {
	.sy_narg	= 1,
	.sy_call	= (sy_call_t *)sys_unregister_event_bell_trap_guarded,
	.sy_auevent	= AUE_NULL,
	.sy_flags	= 0,
};

/*
 * mach_wait_quiet sysent. 1 arg: (timeout) — a uint64_t nanosecond
 * budget (0 == wait indefinitely). Backs IOKitWaitQuiet /
 * IOServiceWaitQuiet.
 */
static struct sysent mach_wait_quiet_sysent = {
	.sy_narg	= 1,
	.sy_call	= (sy_call_t *)sys_mach_wait_quiet_guarded,
	.sy_auevent	= AUE_NULL,
	.sy_flags	= 0,
};

static int mach_reply_port_offset = NO_SYSCALL;
static struct sysent mach_reply_port_old_sysent;

static int task_self_trap_offset = NO_SYSCALL;
static struct sysent task_self_trap_old_sysent;

static int thread_self_trap_offset = NO_SYSCALL;
static struct sysent thread_self_trap_old_sysent;

static int host_self_trap_offset = NO_SYSCALL;
static struct sysent host_self_trap_old_sysent;

static int mach_msg_trap_offset = NO_SYSCALL;
static struct sysent mach_msg_trap_old_sysent;

static int _kernelrpc_mach_port_allocate_offset = NO_SYSCALL;
static struct sysent _kernelrpc_mach_port_allocate_old_sysent;

static int _kernelrpc_mach_port_deallocate_offset = NO_SYSCALL;
static struct sysent _kernelrpc_mach_port_deallocate_old_sysent;

static int _kernelrpc_mach_port_insert_right_offset = NO_SYSCALL;
static struct sysent _kernelrpc_mach_port_insert_right_old_sysent;

static int task_get_special_port_offset = NO_SYSCALL;
static struct sysent task_get_special_port_old_sysent;

static int task_set_special_port_offset = NO_SYSCALL;
static struct sysent task_set_special_port_old_sysent;

static int host_set_special_port_offset = NO_SYSCALL;
static struct sysent host_set_special_port_old_sysent;

static int _kernelrpc_mach_port_move_member_offset = NO_SYSCALL;
static struct sysent _kernelrpc_mach_port_move_member_old_sysent;

static int register_event_bell_offset = NO_SYSCALL;
static struct sysent register_event_bell_old_sysent;

static int unregister_event_bell_offset = NO_SYSCALL;
static struct sysent unregister_event_bell_old_sysent;

static int mach_wait_quiet_offset = NO_SYSCALL;
static struct sysent mach_wait_quiet_old_sysent;

SYSCTL_INT(_mach_syscall, OID_AUTO, mach_reply_port, CTLFLAG_RD,
    &mach_reply_port_offset, 0,
    "Dynamically-allocated FreeBSD syscall number for mach_reply_port "
    "(-1 if registration failed)");

SYSCTL_INT(_mach_syscall, OID_AUTO, task_self_trap, CTLFLAG_RD,
    &task_self_trap_offset, 0,
    "Dynamically-allocated FreeBSD syscall number for task_self_trap "
    "(-1 if registration failed)");

SYSCTL_INT(_mach_syscall, OID_AUTO, thread_self_trap, CTLFLAG_RD,
    &thread_self_trap_offset, 0,
    "Dynamically-allocated FreeBSD syscall number for thread_self_trap "
    "(-1 if registration failed)");

SYSCTL_INT(_mach_syscall, OID_AUTO, host_self_trap, CTLFLAG_RD,
    &host_self_trap_offset, 0,
    "Dynamically-allocated FreeBSD syscall number for host_self_trap "
    "(-1 if registration failed)");

SYSCTL_INT(_mach_syscall, OID_AUTO, mach_msg_trap, CTLFLAG_RD,
    &mach_msg_trap_offset, 0,
    "Dynamically-allocated FreeBSD syscall number for mach_msg_trap "
    "(7-arg syscall; -1 if registration failed)");

/*
 * Port-management traps. sysctl names use the userland-friendly form
 * (mach_port_allocate / _deallocate / _insert_right) so libsystem_kernel's
 * resolve_syscall(name) lookup matches the libxpc-facing function names
 * directly. The kernel-side trap functions retain the Apple
 * `_kernelrpc_*_trap` naming as imported.
 */
SYSCTL_INT(_mach_syscall, OID_AUTO, mach_port_allocate, CTLFLAG_RD,
    &_kernelrpc_mach_port_allocate_offset, 0,
    "Dynamically-allocated FreeBSD syscall number for mach_port_allocate "
    "(3-arg syscall; -1 if registration failed)");

SYSCTL_INT(_mach_syscall, OID_AUTO, mach_port_deallocate, CTLFLAG_RD,
    &_kernelrpc_mach_port_deallocate_offset, 0,
    "Dynamically-allocated FreeBSD syscall number for mach_port_deallocate "
    "(2-arg syscall; -1 if registration failed)");

SYSCTL_INT(_mach_syscall, OID_AUTO, mach_port_insert_right, CTLFLAG_RD,
    &_kernelrpc_mach_port_insert_right_offset, 0,
    "Dynamically-allocated FreeBSD syscall number for mach_port_insert_right "
    "(4-arg syscall; -1 if registration failed)");

/*
 * Task special-port traps. Phase G prerequisite: needed so the
 * bootstrap server can publish its receive port as each client task's
 * TASK_BOOTSTRAP_PORT (and the client can read it back). Apple delivers
 * these over MIG to the task port; we expose them as direct syscalls
 * since freebsd-launchd-mach doesn't ship MIG.
 */
SYSCTL_INT(_mach_syscall, OID_AUTO, task_get_special_port, CTLFLAG_RD,
    &task_get_special_port_offset, 0,
    "Dynamically-allocated FreeBSD syscall number for task_get_special_port "
    "(3-arg syscall; -1 if registration failed)");

SYSCTL_INT(_mach_syscall, OID_AUTO, task_set_special_port, CTLFLAG_RD,
    &task_set_special_port_offset, 0,
    "Dynamically-allocated FreeBSD syscall number for task_set_special_port "
    "(3-arg syscall; -1 if registration failed)");

SYSCTL_INT(_mach_syscall, OID_AUTO, host_set_special_port, CTLFLAG_RD,
    &host_set_special_port_offset, 0,
    "Dynamically-allocated FreeBSD syscall number for host_set_special_port "
    "(3-arg syscall; -1 if registration failed)");

/*
 * mach_port_move_member sysctl name uses the userland-friendly form
 * (matches libmach's resolve_syscall("mach_port_move_member")); the
 * kernel-side handler retains the Apple `_kernelrpc_*_trap` naming.
 */
SYSCTL_INT(_mach_syscall, OID_AUTO, mach_port_move_member, CTLFLAG_RD,
    &_kernelrpc_mach_port_move_member_offset, 0,
    "Dynamically-allocated FreeBSD syscall number for mach_port_move_member "
    "(3-arg syscall; -1 if registration failed)");

SYSCTL_INT(_mach_syscall, OID_AUTO, register_event_bell, CTLFLAG_RD,
    &register_event_bell_offset, 0,
    "Dynamically-allocated FreeBSD syscall number for register_event_bell "
    "(2-arg syscall; -1 if registration failed)");

SYSCTL_INT(_mach_syscall, OID_AUTO, unregister_event_bell, CTLFLAG_RD,
    &unregister_event_bell_offset, 0,
    "Dynamically-allocated FreeBSD syscall number for unregister_event_bell "
    "(1-arg syscall; -1 if registration failed)");

SYSCTL_INT(_mach_syscall, OID_AUTO, mach_wait_quiet, CTLFLAG_RD,
    &mach_wait_quiet_offset, 0,
    "Dynamically-allocated FreeBSD syscall number for mach_wait_quiet "
    "(1-arg syscall; -1 if registration failed)");

static void
wire_one(const char *name, int *offset, struct sysent *sy,
    struct sysent *old_sy)
{
	int error;

	error = kern_syscall_register(sysent, offset, sy, old_sy,
	    SY_THR_STATIC_KLD);
	if (error != 0) {
		printf("mach: syscall_register(%s) failed: %d\n", name, error);
		*offset = NO_SYSCALL;
		return;
	}
	printf("mach: %s registered at syscall %d\n", name, *offset);
}

/*
 * wire_one_at — like wire_one but takes a fixed syscall slot number
 * rather than NO_SYSCALL. Used to claim FreeBSD's RESERVED slots
 * (sys/kern/syscalls.master entries marked "RESERVED reserved for
 * local or vendor use") when the auto-allocated lkmnosys range
 * (210-219) is exhausted. kern_syscall_register accepts an explicit
 * offset and registers if the existing sysent[offset].sy_call is
 * either lkmnosys or nosys (which RESERVED slots use).
 */
static void
wire_one_at(const char *name, int slot, int *offset, struct sysent *sy,
    struct sysent *old_sy)
{
	int error;

	*offset = slot;
	error = kern_syscall_register(sysent, offset, sy, old_sy,
	    SY_THR_STATIC_KLD);
	if (error != 0) {
		printf("mach: syscall_register(%s at %d) failed: %d\n",
		    name, slot, error);
		*offset = NO_SYSCALL;
		return;
	}
	printf("mach: %s registered at syscall %d (explicit)\n",
	    name, *offset);
}

static void
unwire_one(const char *name, int *offset, struct sysent *old_sy)
{
	int error;

	if (*offset == NO_SYSCALL)
		return;
	error = kern_syscall_deregister(sysent, *offset, old_sy);
	if (error != 0)
		printf("mach: syscall_deregister(%s) failed: %d\n", name, error);
}

static void
mach_syscall_wire_register(void *arg __unused)
{

	/*
	 * Pass `sysent` (the global FreeBSD syscall table) as the target.
	 * kern_syscall_register handles offset==NO_SYSCALL specially by
	 * finding a free slot and writing it back.
	 */
	wire_one("mach_reply_port", &mach_reply_port_offset,
	    &mach_reply_port_sysent, &mach_reply_port_old_sysent);
	wire_one("task_self_trap", &task_self_trap_offset,
	    &task_self_trap_sysent, &task_self_trap_old_sysent);
	wire_one("thread_self_trap", &thread_self_trap_offset,
	    &thread_self_trap_sysent, &thread_self_trap_old_sysent);
	wire_one("host_self_trap", &host_self_trap_offset,
	    &host_self_trap_sysent, &host_self_trap_old_sysent);
	wire_one("mach_msg_trap", &mach_msg_trap_offset,
	    &mach_msg_trap_sysent, &mach_msg_trap_old_sysent);
	wire_one("mach_port_allocate", &_kernelrpc_mach_port_allocate_offset,
	    &_kernelrpc_mach_port_allocate_sysent,
	    &_kernelrpc_mach_port_allocate_old_sysent);
	wire_one("mach_port_deallocate", &_kernelrpc_mach_port_deallocate_offset,
	    &_kernelrpc_mach_port_deallocate_sysent,
	    &_kernelrpc_mach_port_deallocate_old_sysent);
	wire_one("mach_port_insert_right", &_kernelrpc_mach_port_insert_right_offset,
	    &_kernelrpc_mach_port_insert_right_sysent,
	    &_kernelrpc_mach_port_insert_right_old_sysent);
	wire_one("task_get_special_port", &task_get_special_port_offset,
	    &task_get_special_port_sysent, &task_get_special_port_old_sysent);
	wire_one("task_set_special_port", &task_set_special_port_offset,
	    &task_set_special_port_sysent, &task_set_special_port_old_sysent);
	wire_one("host_set_special_port", &host_set_special_port_offset,
	    &host_set_special_port_sysent, &host_set_special_port_old_sysent);
	wire_one("mach_port_move_member", &_kernelrpc_mach_port_move_member_offset,
	    &_kernelrpc_mach_port_move_member_sysent,
	    &_kernelrpc_mach_port_move_member_old_sysent);
	wire_one("register_event_bell", &register_event_bell_offset,
	    &register_event_bell_sysent, &register_event_bell_old_sysent);
	wire_one("unregister_event_bell", &unregister_event_bell_offset,
	    &unregister_event_bell_sysent, &unregister_event_bell_old_sysent);
	wire_one("mach_wait_quiet", &mach_wait_quiet_offset,
	    &mach_wait_quiet_sysent, &mach_wait_quiet_old_sysent);
}

static void
mach_syscall_wire_deregister(void *arg __unused)
{

	unwire_one("mach_wait_quiet", &mach_wait_quiet_offset,
	    &mach_wait_quiet_old_sysent);
	unwire_one("unregister_event_bell",
	    &unregister_event_bell_offset,
	    &unregister_event_bell_old_sysent);
	unwire_one("register_event_bell",
	    &register_event_bell_offset,
	    &register_event_bell_old_sysent);
	unwire_one("mach_port_move_member",
	    &_kernelrpc_mach_port_move_member_offset,
	    &_kernelrpc_mach_port_move_member_old_sysent);
	unwire_one("host_set_special_port",
	    &host_set_special_port_offset,
	    &host_set_special_port_old_sysent);
	unwire_one("task_set_special_port",
	    &task_set_special_port_offset,
	    &task_set_special_port_old_sysent);
	unwire_one("task_get_special_port",
	    &task_get_special_port_offset,
	    &task_get_special_port_old_sysent);
	unwire_one("mach_port_insert_right",
	    &_kernelrpc_mach_port_insert_right_offset,
	    &_kernelrpc_mach_port_insert_right_old_sysent);
	unwire_one("mach_port_deallocate",
	    &_kernelrpc_mach_port_deallocate_offset,
	    &_kernelrpc_mach_port_deallocate_old_sysent);
	unwire_one("mach_port_allocate",
	    &_kernelrpc_mach_port_allocate_offset,
	    &_kernelrpc_mach_port_allocate_old_sysent);
	unwire_one("mach_msg_trap", &mach_msg_trap_offset,
	    &mach_msg_trap_old_sysent);
	unwire_one("host_self_trap", &host_self_trap_offset,
	    &host_self_trap_old_sysent);
	unwire_one("thread_self_trap", &thread_self_trap_offset,
	    &thread_self_trap_old_sysent);
	unwire_one("task_self_trap", &task_self_trap_offset,
	    &task_self_trap_old_sysent);
	unwire_one("mach_reply_port", &mach_reply_port_offset,
	    &mach_reply_port_old_sysent);
}

/*
 * Register AFTER SI_SUB_KLD so the IPC zones are up before any caller
 * could hit our handler. Deregister at the same priority on unload.
 */
SYSINIT(mach_syscall_wire, SI_SUB_INTRINSIC, SI_ORDER_ANY,
    mach_syscall_wire_register, NULL);
SYSUNINIT(mach_syscall_wire, SI_SUB_INTRINSIC, SI_ORDER_ANY,
    mach_syscall_wire_deregister, NULL);
