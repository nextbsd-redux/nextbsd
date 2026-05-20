/*
 * mach/mach_traps_mux.h — multiplexer ABI for Mach kernel traps.
 *
 * Background. FreeBSD's syscalls.master reserves exactly 10 lkmnosys
 * slots (210-219) for KLD-allocated syscalls. freebsd-launchd-mach
 * fills all 10 with the foundational Mach trap surface; the eventual
 * launchd+configd+notifyd+asl port wants 20-30 more Mach traps. To
 * fit them without patching the FreeBSD base kernel, we route all
 * NEW Mach traps through a single multiplexer syscall: one FreeBSD
 * slot at a claimed RESERVED-range index (currently slot 91), with
 * an op-number selecting which Mach trap to invoke. Approximates
 * Apple's mach_trap_table architecture one level later in the stack
 * (C handler instead of asm trampoline).
 *
 * Wire contract.
 *   long syscall(MACH_TRAP_MUX_SYSCALL, op, a1, a2, a3, a4, a5);
 *                ^^^^^^^^^^^^^^^^^^^^^  ^^  ^^^^^^^^^^^^^^^^^^
 *                resolved via sysctl    op  up to 5 user args
 *
 * libc syscall() carries up to 6 register args on amd64 (rdi-r9).
 * The op-number uses one; the remaining five are passed through to
 * the underlying Mach trap. Traps needing 6+ args (mach_msg_trap,
 * mach_msg_overwrite_trap) stay in dedicated FreeBSD slots — they
 * don't go through the multiplexer.
 *
 * Op-number stability. Op numbers are stable across releases (unlike
 * the dynamically-assigned FreeBSD syscall slot). Add new ops at the
 * end; never renumber.
 */
#ifndef _MACH_MACH_TRAPS_MUX_H_
#define _MACH_MACH_TRAPS_MUX_H_

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Multiplexer op numbers.
 *
 *   0 — reserved sentinel ("invalid op"); kernel rejects with
 *       KERN_INVALID_ARGUMENT.
 *   1 — host_set_special_port(host, which, port)
 *   2 — task_set_special_port(task, which, port)
 *   3 — _kernelrpc_mach_port_move_member(target, member, after)
 *   4 — mach_event_register_bell(pset_name, write_fd)
 *       Task #39 Path B: registers the write-end of a userland pipe
 *       with a Mach port set. ipc_pset_signal() writes one byte to
 *       the pipe when a message arrives on any port in the set,
 *       waking libdispatch's main kqueue thread (which is registered
 *       for EVFILT_READ on the pipe's read-end). See
 *       src/mach_kmod/src/mach_event_bridge.c.
 *   5 — mach_event_unregister_bell(pset_name)
 *       Task #39 Path B: tears down the bell op 4 registered. The
 *       libmach EVFILT_MACHPORT wrapper calls this when a dispatch
 *       source is torn down (EV_DELETE), BEFORE closing the wakeup
 *       pipe — otherwise the kernel bell outlives the pipe and the
 *       next message's fo_write hits a closed pipe (EPIPE), losing
 *       the wakeup. The bell is otherwise only freed on pset
 *       destroy, which a re-registering source does not trigger.
 *
 * task_set_special_port migrated into the multiplexer to free its
 * previous dedicated FreeBSD syscall slot for the multiplexer
 * itself (which now occupies that slot — FreeBSD's lkmnosys range
 * is exactly 10 entries at 210-219, all used by foundational
 * Mach traps).
 *
 * New ops append at the end. The traps still in dedicated FreeBSD
 * slots (mach_reply_port, task_self_trap, thread_self_trap,
 * host_self_trap, mach_msg_trap, mach_port_allocate, _deallocate,
 * _insert_right, task_get_special_port) do NOT have op numbers —
 * they keep their dedicated slots for stability and (mach_msg_trap)
 * ABI reasons.
 */
#define MACH_TRAP_OP_INVALID			0
#define MACH_TRAP_OP_HOST_SET_SPECIAL_PORT	1
#define MACH_TRAP_OP_TASK_SET_SPECIAL_PORT	2
#define MACH_TRAP_OP_PORT_MOVE_MEMBER		3
#define MACH_TRAP_OP_REGISTER_EVENT_BELL	4
#define MACH_TRAP_OP_UNREGISTER_EVENT_BELL	5

#define MACH_TRAP_OP_MAX			5

#ifdef __cplusplus
}
#endif

#endif /* !_MACH_MACH_TRAPS_MUX_H_ */
