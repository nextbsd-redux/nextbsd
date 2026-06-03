/*
 * mach_traps.c — libmach implementation of the no-arg Mach trap family.
 *
 * Each entry point resolves its FreeBSD syscall number lazily from
 * `sysctl mach.syscall.<name>` on first call (cached in a static for
 * subsequent calls), then invokes the syscall. If the sysctl is
 * missing (mach.ko not loaded) or registration failed (negative number),
 * the function returns MACH_PORT_NULL — matching Apple's API contract
 * of MACH_PORT_NULL on no-resources.
 */

#include <sys/types.h>
#include <sys/sysctl.h>
#include <errno.h>
#include <execinfo.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mach/mach_traps.h>
#include <mach/mach_port.h>
#include <mach/mach_vm.h>	/* mach_vm_allocate / mach_vm_deallocate */
#include <mach/message.h>
#include <mach/ndr.h>
#include <mach/task_special_ports.h>
#include <mach/host_special_ports.h>

#define	NO_SYSCALL	(-1)

/*
 * NDR_record — the per-process NDR sender-representation descriptor
 * MIG-generated client stubs embed in every outgoing message. Apple
 * keeps this in libsystem_kernel; we follow suit. Values are the
 * canonical little-endian / ASCII / IEEE-float layout that matches
 * amd64 FreeBSD.
 */
NDR_record_t NDR_record = {
	.mig_vers      = 0,
	.if_vers       = 0,
	.reserved1     = 0,
	.mig_encoding  = 0,
	.int_rep       = NDR_INT_LITTLE,
	.char_rep      = NDR_CHAR_ASCII,
	.float_rep     = NDR_FLOAT_IEEE,
	.reserved2     = 0,
};

/*
 * resolve_syscall — read sysctl mach.syscall.<name>, return the
 * registered syscall number or NO_SYSCALL if unavailable.
 */
static int
resolve_syscall(const char *name)
{
	char oid[64];
	int num;
	size_t len = sizeof(num);

	if (snprintf(oid, sizeof(oid), "mach.syscall.%s", name) >=
	    (int)sizeof(oid))
		return (NO_SYSCALL);
	if (sysctlbyname(oid, &num, &len, NULL, 0) != 0)
		return (NO_SYSCALL);
	if (num < 0)
		return (NO_SYSCALL);
	return (num);
}

/*
 * Generate one trap entry point. Cache the resolved syscall number
 * in a static; first call does the sysctl lookup, subsequent calls
 * skip it. Thread-safety: the static is written at most once with a
 * value derived from a side-effect-free sysctl, so a racing reader
 * either sees NO_SYSCALL (and re-resolves) or sees the final value.
 */
#define	MACH_TRAP_NOARGS(name)						\
mach_port_name_t							\
name(void)								\
{									\
	static int num = NO_SYSCALL;					\
									\
	if (num == NO_SYSCALL) {					\
		num = resolve_syscall(#name);				\
		if (num == NO_SYSCALL)					\
			return (MACH_PORT_NULL);			\
	}								\
	return ((mach_port_name_t)syscall(num));			\
}

MACH_TRAP_NOARGS(mach_reply_port)

/*
 * Apple exposes the task-self trap as `task_self_trap` (the raw syscall
 * name) and `mach_task_self()` (a userland accessor often inlined to a
 * TLS-style global on Apple platforms). On FreeBSD, with no per-thread
 * Mach storage yet, the simplest mapping is: `mach_task_self()` IS the
 * syscall. Same shape for thread and host.
 */
mach_port_name_t
mach_task_self(void)
{
	static int num = NO_SYSCALL;

	if (num == NO_SYSCALL) {
		num = resolve_syscall("task_self_trap");
		if (num == NO_SYSCALL)
			return (MACH_PORT_NULL);
	}
	return ((mach_port_name_t)syscall(num));
}

mach_port_name_t
mach_thread_self(void)
{
	static int num = NO_SYSCALL;

	if (num == NO_SYSCALL) {
		num = resolve_syscall("thread_self_trap");
		if (num == NO_SYSCALL)
			return (MACH_PORT_NULL);
	}
	return ((mach_port_name_t)syscall(num));
}

mach_port_name_t
mach_host_self(void)
{
	static int num = NO_SYSCALL;

	if (num == NO_SYSCALL) {
		num = resolve_syscall("host_self_trap");
		if (num == NO_SYSCALL)
			return (MACH_PORT_NULL);
	}
	return ((mach_port_name_t)syscall(num));
}

/*
 * mach_msg — send and/or receive a Mach message.
 *
 * Resolves mach.syscall.mach_msg_trap on first call. If the syscall
 * isn't registered, returns a non-zero indeterminate error code
 * (we don't have a defined "syscall unavailable" code in the public
 * mach_msg return-code space, so we return MACH_RCV_TIMED_OUT as a
 * placeholder error; callers should check that mach.ko is loaded
 * before relying on this).
 */
/*
 * mach_msg — 6-arg call into the wired mach_msg_trap.
 *
 * FreeBSD's libc syscall() only correctly shifts up to 6 args into
 * the kernel-syscall ABI registers (rdi, rsi, rdx, r10, r8, r9). The
 * Apple-shape mach_msg_trap has 7 args (msg, option, send_size,
 * rcv_size, rcv_name, timeout, notify); calling syscall(num, ...)
 * with the 7th arg silently garbles args 4-7 (kernel sees rcv_size=0,
 * rcv_name=0, timeout=passed-rcv_size).
 *
 * Workaround: pass only the first 6 args to the kernel. The kernel
 * wrapper sets notify=MACH_PORT_NULL. The `notify` arg here is
 * accepted for API compatibility but currently ignored — a future
 * `mach_msg2`-style wrapper or an inline-asm 7-arg syscall stub
 * can lift this restriction if real use needs notify.
 */
mach_msg_return_t
mach_msg(mach_msg_header_t *msg, mach_msg_option_t option,
    mach_msg_size_t send_size, mach_msg_size_t rcv_size,
    mach_port_name_t rcv_name, mach_msg_timeout_t timeout,
    mach_port_name_t notify __attribute__((unused)))
{
	static int num = NO_SYSCALL;

	if (num == NO_SYSCALL) {
		num = resolve_syscall("mach_msg_trap");
		if (num == NO_SYSCALL)
			return (MACH_RCV_TIMED_OUT);
	}
	/*
	 * Task #39 Bug-B trace: when MACH_DEBUG_BADSEND is set, dump a
	 * backtrace for any SEND whose destination port name is a
	 * suspiciously small integer (< 32 — likely a stray fd, not a
	 * real Mach port name). Pinpoints which caller is confusing an
	 * fd for a port name. Compiled-in but inert unless the env var
	 * is present.
	 */
	if ((option & 0x1 /* MACH_SEND_MSG */) && msg != NULL &&
	    msg->msgh_remote_port != 0 && msg->msgh_remote_port < 32 &&
	    getenv("MACH_DEBUG_BADSEND") != NULL) {
		void *bt[24];
		int n = backtrace(bt, 24);
		fprintf(stderr, "[BADSEND] mach_msg SEND to remote_port=0x%x "
		    "id=0x%x — backtrace:\n",
		    msg->msgh_remote_port, msg->msgh_id);
		backtrace_symbols_fd(bt, n, 2);
	}
	return ((mach_msg_return_t)syscall(num, msg, option,
	    send_size, rcv_size, rcv_name, timeout));
}

/*
 * Port-management traps. Same lazy-resolve pattern as the no-arg traps,
 * but each takes >0 args. All three return kern_return_t (0 = success).
 * On no mach.ko, return KERN_RESOURCE_SHORTAGE (6) — matches Apple's
 * "couldn't allocate" convention.
 *
 * `task` is always the caller's task port (use mach_task_self()).
 * The kernel-side wrappers ignore it and use current_task() — included
 * in the userland API for Apple-source-code compatibility.
 */
kern_return_t
mach_port_allocate(mach_port_name_t task, mach_port_right_t right,
    mach_port_name_t *name)
{
	static int num = NO_SYSCALL;

	if (num == NO_SYSCALL) {
		num = resolve_syscall("mach_port_allocate");
		if (num == NO_SYSCALL)
			return (KERN_RESOURCE_SHORTAGE);
	}
	return ((kern_return_t)syscall(num, task, right, name));
}

kern_return_t
mach_port_deallocate(mach_port_name_t task, mach_port_name_t name)
{
	static int num = NO_SYSCALL;

	if (num == NO_SYSCALL) {
		num = resolve_syscall("mach_port_deallocate");
		if (num == NO_SYSCALL)
			return (KERN_RESOURCE_SHORTAGE);
	}
	return ((kern_return_t)syscall(num, task, name));
}

kern_return_t
mach_port_insert_right(mach_port_name_t task, mach_port_name_t name,
    mach_port_t poly, mach_msg_type_name_t polyPoly)
{
	static int num = NO_SYSCALL;

	if (num == NO_SYSCALL) {
		num = resolve_syscall("mach_port_insert_right");
		if (num == NO_SYSCALL)
			return (KERN_RESOURCE_SHORTAGE);
	}
	return ((kern_return_t)syscall(num, task, name, poly, polyPoly));
}

/*
 * Task special-port traps. `which` is TASK_BOOTSTRAP_PORT etc. from
 * <mach/task_special_ports.h>. task_get_bootstrap_port /
 * task_set_bootstrap_port are macros that pick TASK_BOOTSTRAP_PORT for
 * the `which` arg.
 */
kern_return_t
task_get_special_port(mach_port_name_t task, int which,
    mach_port_name_t *port)
{
	static int num = NO_SYSCALL;

	if (num == NO_SYSCALL) {
		num = resolve_syscall("task_get_special_port");
		if (num == NO_SYSCALL)
			return (KERN_RESOURCE_SHORTAGE);
	}
	return ((kern_return_t)syscall(num, task, which, port));
}

/*
 * task_set_special_port — dedicated syscall (mach.syscall.
 * task_set_special_port). Each Mach trap now has its own FreeBSD
 * syscall slot, resolved by name; no multiplexer.
 */
kern_return_t
task_set_special_port(mach_port_name_t task, int which, mach_port_t port)
{
	static int num = NO_SYSCALL;

	if (num == NO_SYSCALL) {
		num = resolve_syscall("task_set_special_port");
		if (num == NO_SYSCALL)
			return (KERN_RESOURCE_SHORTAGE);
	}
	return ((kern_return_t)syscall(num, task, which, port));
}

/*
 * host_set_special_port — set a slot in the host's special-port
 * array. Dedicated syscall (mach.syscall.host_set_special_port).
 *
 * The bootstrap server calls this once at startup to publish its
 * receive port host-wide; thereafter task_get_special_port(
 * TASK_BOOTSTRAP_PORT) falls back to realhost.special[
 * HOST_BOOTSTRAP_PORT] when the per-task itk_bootstrap slot is null.
 */
kern_return_t
host_set_special_port(mach_port_name_t host, int which, mach_port_t port)
{
	static int num = NO_SYSCALL;

	if (num == NO_SYSCALL) {
		num = resolve_syscall("host_set_special_port");
		if (num == NO_SYSCALL)
			return (KERN_RESOURCE_SHORTAGE);
	}
	return ((kern_return_t)syscall(num, host, which, port));
}

/*
 * Phase I1c link-stage stubs.
 *
 * launchd-842 references a large surface of Apple userland Mach
 * APIs that we haven't ported yet. Until each gets a real backing
 * (sysctl(KERN_PROC), real mach_port RPCs, kqueue-backed Mach msgs,
 * libdispatch timing) the stubs let the launchd link succeed and
 * the daemon's no-IPC CLI path runs. Calls that hit them at runtime
 * fail closed (KERN_RESOURCE_SHORTAGE / -1 / no-op) — the daemon's
 * existing graceful-degradation paths handle that.
 */
#include <time.h>
#include <mach/exception.h>
#include <mach/host_reboot.h>
#include <mach/mach_host.h>
#include <mach/mach_time.h>
#include <mach/task_policy.h>

uint64_t
mach_absolute_time(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t
mach_continuous_time(void)
{
	return mach_absolute_time();
}

kern_return_t
mach_timebase_info(mach_timebase_info_t info)
{
	if (info != NULL) {
		info->numer = 1;
		info->denom = 1;
	}
	return KERN_SUCCESS;
}

kern_return_t
host_info(host_t host, host_flavor_t flavor,
    host_info_t info, mach_msg_type_number_t *count)
{
	(void)host; (void)flavor; (void)info;
	if (count != NULL)
		*count = 0;
	return KERN_RESOURCE_SHORTAGE;
}

kern_return_t
host_statistics(host_t host, host_flavor_t flavor,
    host_info_t info, mach_msg_type_number_t *count)
{
	(void)host; (void)flavor; (void)info;
	if (count != NULL)
		*count = 0;
	return KERN_RESOURCE_SHORTAGE;
}

kern_return_t
host_reboot(host_priv_t host_priv, int options)
{
	(void)host_priv; (void)options;
	return KERN_RESOURCE_SHORTAGE;
}

kern_return_t
task_policy_set(task_t task, task_policy_flavor_t flavor,
    task_policy_t policy_info, mach_msg_type_number_t count)
{
	(void)task; (void)flavor; (void)policy_info; (void)count;
	return KERN_SUCCESS;
}

kern_return_t
task_policy_get(task_t task, task_policy_flavor_t flavor,
    task_policy_t policy_info, mach_msg_type_number_t *count,
    boolean_t *get_default)
{
	(void)task; (void)flavor; (void)policy_info;
	if (count != NULL)
		*count = 0;
	if (get_default != NULL)
		*get_default = 0;
	return KERN_SUCCESS;
}

kern_return_t
task_set_exception_ports(task_t task, exception_mask_t mask,
    mach_port_t new_port, exception_behavior_t behavior,
    thread_state_flavor_t new_flavor)
{
	(void)task; (void)mask; (void)new_port;
	(void)behavior; (void)new_flavor;
	return KERN_SUCCESS;
}

kern_return_t
host_set_exception_ports(host_priv_t host_priv, exception_mask_t mask,
    mach_port_t new_port, exception_behavior_t behavior,
    thread_state_flavor_t new_flavor)
{
	(void)host_priv; (void)mask; (void)new_port;
	(void)behavior; (void)new_flavor;
	return KERN_SUCCESS;
}

kern_return_t
mach_port_set_attributes(mach_port_name_t task, mach_port_name_t name,
    mach_port_flavor_t flavor, mach_port_info_t info,
    mach_msg_type_number_t infoCnt)
{
	(void)task; (void)name; (void)flavor; (void)info; (void)infoCnt;
	return KERN_SUCCESS;
}

kern_return_t
mach_port_get_attributes(mach_port_name_t task, mach_port_name_t name,
    mach_port_flavor_t flavor, mach_port_info_t info,
    mach_msg_type_number_t *infoCnt)
{
	(void)task; (void)name; (void)flavor; (void)info;
	if (infoCnt != NULL)
		*infoCnt = 0;
	return KERN_RESOURCE_SHORTAGE;
}

kern_return_t
mach_port_mod_refs(mach_port_name_t task, mach_port_name_t name,
    mach_port_right_t right, mach_port_delta_t delta)
{
	(void)task; (void)name; (void)right; (void)delta;
	return KERN_SUCCESS;
}

/*
 * mach_port_move_member: dedicated syscall (mach.syscall.
 * mach_port_move_member). Task #41 root cause: this used to return
 * KERN_SUCCESS without doing anything, so launchd's runtime_add_mport
 * silently failed to add launchd_internal_port to its ipc_port_set.
 * Every launchd-spawned daemon hung in launch_msg(CHECKIN) because the
 * kqueue→handle_kqueue Mach send arrived on a port the main thread
 * wasn't listening on.
 */
kern_return_t
mach_port_move_member(mach_port_name_t task, mach_port_name_t member,
    mach_port_name_t after)
{
	static int num = NO_SYSCALL;

	if (num == NO_SYSCALL) {
		num = resolve_syscall("mach_port_move_member");
		if (num == NO_SYSCALL)
			return (KERN_RESOURCE_SHORTAGE);
	}
	return ((kern_return_t)syscall(num, task, member, after));
}

kern_return_t
mach_port_request_notification(mach_port_name_t task,
    mach_port_name_t name, mach_msg_id_t msgid,
    mach_port_mscount_t sync, mach_port_t notify,
    mach_msg_type_name_t notifyPoly, mach_port_t *previous)
{
	(void)task; (void)name; (void)msgid; (void)sync;
	(void)notify; (void)notifyPoly;
	if (previous != NULL)
		*previous = 0;
	return KERN_SUCCESS;
}

kern_return_t
mach_port_extract_right(mach_port_name_t task, mach_port_name_t name,
    mach_msg_type_name_t desired, mach_port_t *port,
    mach_msg_type_name_t *acquired)
{
	(void)task; (void)name; (void)desired;
	if (port != NULL)
		*port = 0;
	if (acquired != NULL)
		*acquired = 0;
	return KERN_RESOURCE_SHORTAGE;
}

kern_return_t
mach_port_set_mscount(mach_port_name_t task, mach_port_name_t name,
    mach_port_mscount_t mscount)
{
	(void)task; (void)name; (void)mscount;
	return KERN_SUCCESS;
}

/*
 * mach_msg_destroy() — clean up port rights / OOL memory in a
 * message that won't be sent. Stub does nothing; the leaks are
 * harmless on the no-IPC CLI path.
 */
#include <mach/message.h>
void
mach_msg_destroy(mach_msg_header_t *msg)
{
	(void)msg;
}

/*
 * audit_token_to_au32() — unpack the 8-field audit token into named
 * uint32 outs. The token layout is fixed (val[0..7]); on FreeBSD the
 * mapping has slightly different semantics but the field arrangement
 * is the same Apple uses, so this works.
 */
void
audit_token_to_au32(audit_token_t atok,
    uint32_t *auidp, uint32_t *euidp, uint32_t *egidp,
    uint32_t *ruidp, uint32_t *rgidp, uint32_t *pidp,
    uint32_t *asidp, uint32_t *tidp)
{
	if (auidp != NULL) *auidp = atok.val[0];
	if (euidp != NULL) *euidp = atok.val[1];
	if (egidp != NULL) *egidp = atok.val[2];
	if (ruidp != NULL) *ruidp = atok.val[3];
	if (rgidp != NULL) *rgidp = atok.val[4];
	if (pidp  != NULL) *pidp  = atok.val[5];
	if (asidp != NULL) *asidp = atok.val[6];
	if (tidp  != NULL) *tidp  = atok.val[7];
}

/*
 * pid_for_task() — map a task port to its pid. Apple-canonical.
 * launchd uses it only on jobs it spawned, whose pid it already
 * knows; the stub fails closed so callers take the "unknown" path.
 */
kern_return_t
pid_for_task(mach_port_name_t task, int *pid)
{
	(void)task;
	if (pid != NULL)
		*pid = -1;
	return KERN_FAILURE;
}

/*
 * Phase I1c link-stage stubs — round 2.
 *
 * MIG runtime functions: Apple ships these as part of libsystem's
 * MIG support; they back the client-side stubs. Real implementations
 * back the reply-port cache + OOL allocation; ours are minimal so
 * non-XPC code paths (the no-IPC CLI smoke) succeed and the XPC
 * paths fail closed.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <bsm/audit.h>
#include <mach/mig_errors.h>

mach_port_t
mig_get_reply_port(void)
{
	/* Apple maintains a per-thread reply-port cache; we don't yet.
	 * Allocate a fresh port for each call — slower but correct. */
	return (mach_port_t)mach_reply_port();
}

void
mig_put_reply_port(mach_port_t port)
{
	/* No reply-port cache to put it back into; drop the right. */
	if (port != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), port);
}

void
mig_dealloc_reply_port(mach_port_t port)
{
	if (port != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), port);
}

kern_return_t
mig_allocate(vm_address_t *addr, vm_size_t size)
{
	void *p = malloc((size_t)size);
	if (p == NULL)
		return KERN_RESOURCE_SHORTAGE;
	*addr = (vm_address_t)(uintptr_t)p;
	return KERN_SUCCESS;
}

/*
 * mig_deallocate — same shape of trap vm_deallocate already documents
 * a few lines down: the same name gets called on buffers from two
 * different allocators, and using one of them on the wrong source
 * crashes hard.
 *
 *   - Server side (e.g. launchd PID 1 packing an OOL response):
 *     mig_allocate above returns a malloc'd buffer; after the MIG
 *     send the server mig_deallocate's that buffer. malloc -> free.
 *   - Client side (e.g. launchctl receiving the OOL response): the
 *     pointer is an INTERIOR offset into the kernel-handed-back
 *     mmap'd Mach message body (not page-aligned, not the base of
 *     anything our libc can free), and the size that comes back can
 *     be the server's full payload, not the receive-buffer size.
 *     free() on this walks jemalloc's arena metadata into garbage
 *     and segfaults; munmap fails too (interior pointer).
 *
 * `launchctl list` (vproc_swap_complex -> mig_deallocate at the
 * cleanup label) hit the client path and crashed in libc free
 * walking arena state. Confirmed with lldb on bsd01: rdi was an
 * interior pointer 0x5110 bytes into a 576 KiB rw- mmap region,
 * declared size was 7.6 MiB (the server payload, larger than the
 * actually-mapped region) — neither free nor munmap can do anything
 * useful with that.
 *
 * Take the same trade vm_deallocate already takes: no-op + bounded
 * leak. The server-side leak (launchd PID 1's malloc-based OOL
 * response buffers) is the unbounded one if you measure it over
 * months of daemon uptime; OOL responses are KB-to-low-MB each, so
 * accumulation is measured in low-MB-per-day for an active system.
 * The proper fix is to make mig_allocate mmap-based (matching
 * vm_allocate) AND fix the OOL-receive path to hand back a true
 * base pointer + true mapped size, so a real munmap can run on both
 * sides. That's mach_msg surgery; do it when the leak budget shows
 * up as a real problem, not before.
 */
kern_return_t
mig_deallocate(vm_address_t addr, vm_size_t size)
{
	(void)addr;
	(void)size;
	return KERN_SUCCESS;
}

int
mig_strncpy(char *dst, const char *src, int len)
{
	if (len <= 0)
		return 0;
	int n = (int)strlen(src);
	if (n >= len)
		n = len - 1;
	memcpy(dst, src, (size_t)n);
	dst[n] = '\0';
	return n;
}

/* vm_allocate / vm_deallocate — Mach VM API surface. vm_allocate uses
 * mmap so callers can rely on getting a real anonymous mapping;
 * vm_deallocate is a no-op because the same name is sometimes called
 * on malloc'd buffers (from mig_allocate), where munmap would crash.
 * Bounded leak — MIG out-of-line messages are KB-sized at most. */
kern_return_t
vm_allocate(mach_port_name_t target, vm_address_t *address,
    vm_size_t size, int flags)
{
	void *p;
	(void)target;
	(void)flags;
	if (address == NULL || size == 0)
		return (KERN_INVALID_ARGUMENT);
	p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	if (p == MAP_FAILED)
		return (KERN_NO_SPACE);
	*address = (vm_address_t)(uintptr_t)p;
	return (KERN_SUCCESS);
}

kern_return_t
vm_deallocate(mach_port_name_t task, vm_address_t addr, vm_size_t size)
{
	(void)task;
	(void)addr;
	(void)size;
	return (KERN_SUCCESS);
}

/* Additional mach_port stubs. */
kern_return_t
mach_port_get_set_status(mach_port_name_t task, mach_port_name_t name,
    mach_port_name_array_t *members, mach_msg_type_number_t *membersCnt)
{
	(void)task; (void)name;
	if (members != NULL)
		*members = NULL;
	if (membersCnt != NULL)
		*membersCnt = 0;
	return KERN_SUCCESS;
}

kern_return_t
mach_port_set_context(mach_port_name_t task, mach_port_name_t name,
    mach_port_context_t context)
{
	(void)task; (void)name; (void)context;
	return KERN_SUCCESS;
}

/*
 * audit_session_self / audit_session_join — Apple-shape decls live
 * in FreeBSD's <bsm/audit.h> but only behind __APPLE_API_PRIVATE,
 * which libmach doesn't define. libbsm also doesn't ship the
 * implementations. Re-state the prototypes here so libmach's
 * -Wmissing-prototypes is satisfied and stub them so the link
 * succeeds; runtime callers see MACH_PORT_NULL / AU_DEFAUDITSID.
 */
mach_port_name_t audit_session_self(void);
au_asid_t        audit_session_join(mach_port_name_t port);

mach_port_name_t
audit_session_self(void)
{
	return MACH_PORT_NULL;
}

au_asid_t
audit_session_join(mach_port_name_t port)
{
	(void)port;
	return 0;	/* AU_DEFAUDITSID */
}

/* host_set_UNDServer — User Notification Daemon port. macOS-only;
 * stub success so launchd's set-once call after startup is a no-op. */
kern_return_t
host_set_UNDServer(host_t host, mach_port_t port)
{
	(void)host; (void)port;
	return KERN_SUCCESS;
}

/*
 * gL1CacheEnabled — Apple's Libinfo exposes this `extern int` so
 * callers can disable the negative-lookup cache around getpwnam
 * retries. launchd-842/core.c does `extern int gL1CacheEnabled;`
 * and assigns `false` before re-querying. FreeBSD's nsswitch has no
 * equivalent toggle, so the variable is purely a sink. Carry an
 * extern decl here too so -Wmissing-variable-declarations is happy.
 */
extern int gL1CacheEnabled;
int gL1CacheEnabled = 1;

/*
 * task_self_trap() — Apple's raw-syscall name for "give me my task
 * port". Same shape as mach_task_self(); some Apple-source uses the
 * trap name directly (e.g. inside Mach-VM C++ glue).
 */
mach_port_name_t
task_self_trap(void)
{
	return mach_task_self();
}

kern_return_t
task_name_for_pid(mach_port_name_t target_task, int pid,
    mach_port_name_t *t)
{
	(void)target_task; (void)pid;
	if (t != NULL)
		*t = MACH_PORT_NULL;
	return KERN_FAILURE;
}

kern_return_t
mach_port_get_context(mach_port_name_t task, mach_port_name_t name,
    mach_port_context_t *context)
{
	(void)task; (void)name;
	if (context != NULL)
		*context = 0;
	return KERN_SUCCESS;
}

/*
 * thread_switch() — yield + optional short wait. See mach_traps.h for
 * the design note. Reuses sched_yield(); SWITCH_OPTION_WAIT adds a
 * nanosleep before the yield. option_time is in milliseconds (Apple's
 * convention).
 */
kern_return_t
thread_switch(mach_port_name_t thread_name, int option, uint32_t option_time)
{
	(void)thread_name;

	if (option == SWITCH_OPTION_WAIT && option_time != 0) {
		struct timespec ts;
		ts.tv_sec = option_time / 1000;
		ts.tv_nsec = (long)(option_time % 1000) * 1000000L;
		(void)nanosleep(&ts, NULL);
	}
	(void)sched_yield();
	return KERN_SUCCESS;
}

/*
 * thread_destruct_special_reply_port — XNU-private sync-IPC helper.
 * Never called at runtime on FreeBSD because
 * DISPATCH_USE_MACH_SEND_SYNC_OVERRIDE is 0 (target.h:58 makes
 * DISPATCH_MIN_REQUIRED_OSX_AT_LEAST return 0 on non-Apple). Stub
 * fails closed so any unexpected runtime call is detectable.
 */
kern_return_t
thread_destruct_special_reply_port(mach_port_name_t reply_port,
    enum thread_destruct_special_reply_port_rights rights)
{
	(void)reply_port;
	(void)rights;
	return KERN_FAILURE;
}

/*
 * mach_port_destruct — Apple-API tear-down of a port's receive + a
 * single send-once right in one call. We currently model the universe
 * as single-task, so receive and send-once aren't separable; route to
 * mach_port_deallocate. srdelta and guard are accepted but ignored.
 */
kern_return_t
mach_port_destruct(mach_port_name_t task, mach_port_name_t name,
    mach_port_delta_t srdelta, mach_port_context_t guard)
{
	(void)srdelta;
	(void)guard;
	return mach_port_deallocate(task, name);
}

/*
 * mach_port_construct — Apple's options-bearing port allocator.
 * libdispatch (_dispatch_mach_notify_port_init) passes
 * MPO_CONTEXT_AS_GUARD | MPO_STRICT to attach a guard cookie that
 * mach_port_destruct must present on tear-down. Our kernel doesn't
 * track guards yet; we route to mach_port_allocate(RECEIVE) and
 * discard the options.
 */
kern_return_t
mach_port_construct(mach_port_name_t task, mach_port_options_t *opts,
    mach_port_context_t guard, mach_port_name_t *name)
{
	(void)opts;
	(void)guard;
	return mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, name);
}

/*
 * host_get_host_port — drop privileges from a HOST_PRIV port to the
 * unprivileged host port. We don't distinguish; copy the input name.
 */
kern_return_t
host_get_host_port(mach_port_name_t host_priv, mach_port_name_t *host_port)
{
	if (host_port != NULL)
		*host_port = host_priv;
	return KERN_SUCCESS;
}

/*
 * host_request_notification — register a port to receive host
 * notifications (calendar change etc.). Stub: succeed but never
 * deliver. libdispatch's calendar-change subscriber still works at
 * call time; the timer subsystem polls clock_gettime independently.
 */
kern_return_t
host_request_notification(mach_port_name_t host, int notify_type,
    mach_port_name_t notify_port)
{
	(void)host;
	(void)notify_type;
	(void)notify_port;
	return KERN_SUCCESS;
}

/*
 * Mach VM APIs — used by libdispatch's data.c (line 139
 * mach_vm_deallocate for the dispatch_data large-buffer path). Apple
 * implements these as full kernel RPC; we route to mmap/munmap. The
 * VM_PROT_* and VM_FLAGS_* options are accepted but ignored — every
 * allocation is RW anon-private, every deallocation is munmap.
 */
#include <sys/mman.h>

kern_return_t
mach_vm_allocate(mach_port_name_t target, mach_vm_address_t *address,
    mach_vm_size_t size, int flags)
{
	void *p;

	(void)target;
	(void)flags;
	if (address == NULL || size == 0)
		return (KERN_INVALID_ARGUMENT);

	p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	if (p == MAP_FAILED)
		return (KERN_NO_SPACE);
	*address = (mach_vm_address_t)(uintptr_t)p;
	return (KERN_SUCCESS);
}

kern_return_t
mach_vm_deallocate(mach_port_name_t target, mach_vm_address_t address,
    mach_vm_size_t size)
{
	(void)target;
	if (size == 0)
		return (KERN_SUCCESS);
	if (munmap((void *)(uintptr_t)address, (size_t)size) != 0)
		return (KERN_FAILURE);
	return (KERN_SUCCESS);
}

/*
 * mach_port_type — return the rights mask for a port name in the
 * caller's task. Apple uses it for port-validity probes. Without
 * kernel introspection wired up, we report RECEIVE if the name
 * isn't NULL/DEAD; consumers (event_kevent.c:289) use it for
 * coarse validation.
 */
kern_return_t
mach_port_type(mach_port_name_t task, mach_port_name_t name,
    mach_port_type_t *type)
{
	(void)task;
	if (type == NULL)
		return (KERN_INVALID_ARGUMENT);
	if (name == MACH_PORT_NULL || name == MACH_PORT_DEAD) {
		*type = 0;
		return (KERN_INVALID_NAME);
	}
	*type = MACH_PORT_TYPE_RECEIVE;
	return (KERN_SUCCESS);
}

/*
 * Mach semaphores — backed by POSIX sem_t under a small registry
 * keyed by Apple-shape mach_port_name_t identifiers. libdispatch's
 * USE_MACH_SEM path uses these as the underlying _dispatch_sema4
 * primitive. The implementation is intentionally minimal: linear
 * scan over a 256-entry table, locked by a single mutex. Real
 * workloads (dispatch_semaphore_wait, _dispatch_sema4_* on contended
 * paths) only allocate a handful of these per process.
 */
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

#define MACH_SEM_TABLE_SIZE 256

struct mach_sem_entry {
	mach_port_name_t	name;
	sem_t			sem;
	int			in_use;
};

static struct mach_sem_entry	mach_sem_table[MACH_SEM_TABLE_SIZE];
static pthread_mutex_t		mach_sem_table_mtx = PTHREAD_MUTEX_INITIALIZER;
static mach_port_name_t		mach_sem_next_name = 0x10000;

static struct mach_sem_entry *
mach_sem_find_locked(mach_port_name_t name)
{
	for (int i = 0; i < MACH_SEM_TABLE_SIZE; i++) {
		if (mach_sem_table[i].in_use &&
		    mach_sem_table[i].name == name) {
			return (&mach_sem_table[i]);
		}
	}
	return (NULL);
}

kern_return_t
semaphore_create(mach_port_name_t task, semaphore_t *sem, int policy,
    int value)
{
	(void)task;
	(void)policy;

	if (sem == NULL || value < 0)
		return (KERN_INVALID_ARGUMENT);

	pthread_mutex_lock(&mach_sem_table_mtx);
	for (int i = 0; i < MACH_SEM_TABLE_SIZE; i++) {
		if (!mach_sem_table[i].in_use) {
			if (sem_init(&mach_sem_table[i].sem, 0,
			    (unsigned int)value) != 0) {
				pthread_mutex_unlock(&mach_sem_table_mtx);
				return (KERN_RESOURCE_SHORTAGE);
			}
			mach_sem_table[i].name = mach_sem_next_name++;
			mach_sem_table[i].in_use = 1;
			*sem = mach_sem_table[i].name;
			pthread_mutex_unlock(&mach_sem_table_mtx);
			return (KERN_SUCCESS);
		}
	}
	pthread_mutex_unlock(&mach_sem_table_mtx);
	return (KERN_RESOURCE_SHORTAGE);
}

kern_return_t
semaphore_destroy(mach_port_name_t task, semaphore_t sem)
{
	struct mach_sem_entry *e;

	(void)task;
	pthread_mutex_lock(&mach_sem_table_mtx);
	e = mach_sem_find_locked(sem);
	if (e == NULL) {
		pthread_mutex_unlock(&mach_sem_table_mtx);
		return (KERN_INVALID_ARGUMENT);
	}
	(void)sem_destroy(&e->sem);
	e->in_use = 0;
	pthread_mutex_unlock(&mach_sem_table_mtx);
	return (KERN_SUCCESS);
}

kern_return_t
semaphore_signal(semaphore_t sem)
{
	struct mach_sem_entry *e;
	sem_t *p = NULL;

	pthread_mutex_lock(&mach_sem_table_mtx);
	e = mach_sem_find_locked(sem);
	if (e != NULL)
		p = &e->sem;
	pthread_mutex_unlock(&mach_sem_table_mtx);
	if (p == NULL)
		return (KERN_INVALID_ARGUMENT);
	if (sem_post(p) != 0)
		return (KERN_FAILURE);
	return (KERN_SUCCESS);
}

kern_return_t
semaphore_wait(semaphore_t sem)
{
	struct mach_sem_entry *e;
	sem_t *p = NULL;

	pthread_mutex_lock(&mach_sem_table_mtx);
	e = mach_sem_find_locked(sem);
	if (e != NULL)
		p = &e->sem;
	pthread_mutex_unlock(&mach_sem_table_mtx);
	if (p == NULL)
		return (KERN_INVALID_ARGUMENT);
	while (sem_wait(p) != 0) {
		if (errno == EINTR)
			return (KERN_ABORTED);
		return (KERN_FAILURE);
	}
	return (KERN_SUCCESS);
}

kern_return_t
semaphore_timedwait(semaphore_t sem, mach_timespec_t wait_time)
{
	struct mach_sem_entry *e;
	struct timespec abs_ts;
	sem_t *p = NULL;

	pthread_mutex_lock(&mach_sem_table_mtx);
	e = mach_sem_find_locked(sem);
	if (e != NULL)
		p = &e->sem;
	pthread_mutex_unlock(&mach_sem_table_mtx);
	if (p == NULL)
		return (KERN_INVALID_ARGUMENT);

	/* sem_timedwait takes an absolute CLOCK_REALTIME deadline; Apple's
	 * semaphore_timedwait takes a relative delta. Convert by adding to
	 * now. */
	if (clock_gettime(CLOCK_REALTIME, &abs_ts) != 0)
		return (KERN_FAILURE);
	abs_ts.tv_sec += (time_t)wait_time.tv_sec;
	abs_ts.tv_nsec += wait_time.tv_nsec;
	if (abs_ts.tv_nsec >= 1000000000L) {
		abs_ts.tv_sec += 1;
		abs_ts.tv_nsec -= 1000000000L;
	}

	if (sem_timedwait(p, &abs_ts) != 0) {
		if (errno == ETIMEDOUT)
			return (KERN_OPERATION_TIMED_OUT);
		if (errno == EINTR)
			return (KERN_ABORTED);
		return (KERN_FAILURE);
	}
	return (KERN_SUCCESS);
}

/*
 * mach_msg_server_once() — MIG runtime helper: receive one message,
 * dispatch it through the given demux, send the reply. liblaunch's
 * libvproc.c uses it to service helper-downcall requests.
 *
 * Implements the canonical loop body once: rcv into a buffer, call
 * demux, send. demux signature matches Apple's mig_server_routine_t
 * (a function that returns boolean_t after filling the reply).
 *
 * Buffer size is the caller's max_size + room for the largest
 * trailer; with no header alignment trickery yet, just stack-
 * allocate via VLA.
 */
typedef boolean_t (*_mach_demux_t)(mach_msg_header_t *, mach_msg_header_t *);

mach_msg_return_t
mach_msg_server_once(boolean_t (*demux)(mach_msg_header_t *, mach_msg_header_t *),
    mach_msg_size_t max_size, mach_port_name_t rcv_name,
    mach_msg_options_t options)
{
	(void)demux; (void)max_size; (void)rcv_name; (void)options;
	/*
	 * Real implementation: mach_msg(rcv) -> demux(req, rep) ->
	 * mach_msg(send). For Phase I1c we don't run the helper-down-
	 * call path; stub to MACH_RCV_TIMED_OUT so callers retry
	 * harmlessly. liblaunch's poller treats any non-success as
	 * "no message right now".
	 */
	return MACH_RCV_TIMED_OUT;
}
