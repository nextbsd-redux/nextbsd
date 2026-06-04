/*
 * _mach_sysproto.h — Mach syscall argument struct definitions, extracted
 * from ravynOS sys/sys/sysproto.h (auto-generated from their
 * syscalls.master). Used by mach_traps.c / mach_misc.c / mach_module.c
 * to declare the *_args containers SYSCALL_INIT_HELPER references.
 *
 * PADL_/PADR_ are redefined to 0 here. The canonical FreeBSD
 * definitions handle 32-on-64-bit syscall ABI padding; for our
 * purposes (kmod build, syscalls registered with NO_SYSCALL so they're
 * dynamically slotted and not invoked from userland) the padding is
 * irrelevant and would require register_t to be in scope.
 */
#ifndef	_MACH_KMOD__MACH_SYSPROTO_H_
#define	_MACH_KMOD__MACH_SYSPROTO_H_

/*
 * Pull in the Mach typedefs the struct fields reference. Idempotent
 * inclusion; safe even if the caller already brought them in.
 */
#include <sys/mach/mach_types.h>
#include <sys/mach/message.h>

/*
 * PADL_/PADR_ alignment macros. The canonical FreeBSD versions in
 * <sys/sysproto.h> pad each syscall arg field out to register_t-size
 * (8 bytes on amd64) so that the kernel's argument fetcher (which
 * writes register_t-sized slots into the struct) lines up with the
 * struct field offsets.
 *
 * Originally we stubbed these to 0 because we thought the syscalls
 * weren't being invoked from userland (NO_SYSCALL meant the helper-
 * register loop terminated immediately). Once we wired the trap
 * family via kern_syscall_register, that became false — multi-arg
 * syscalls like mach_msg_trap now ARE invoked from userland, and the
 * 0/0 stubs produce a PACKED struct whose 4-byte fields don't line
 * up with the kernel's 8-byte slot writes (rcv_size winds up at
 * struct offset 16 while the kernel writes args[2] at offset 16
 * containing the previous arg — values bleed across fields).
 *
 * Use the canonical PADL_/PADR_ from <sys/sysproto.h> if available
 * (mach_traps.c et al. include sys/sysproto.h before reaching here).
 * Otherwise replicate the standard little-endian definition.
 */
#include <machine/_types.h>		/* __register_t */
#ifndef	PADL_
#define	_MACH_PAD_(t)	(sizeof(__register_t) <= sizeof(t) \
				? 0 : sizeof(__register_t) - sizeof(t))
#if BYTE_ORDER == LITTLE_ENDIAN
#define	PADL_(t)	0
#define	PADR_(t)	_MACH_PAD_(t)
#else
#define	PADL_(t)	_MACH_PAD_(t)
#define	PADR_(t)	0
#endif
#endif


/* From ravynOS sys/sys/sysproto.h lines 701-712 (__proc_info, __iopolicysys) */
struct __proc_info_args {
	char callnum_l_[PADL_(int)]; int callnum; char callnum_r_[PADR_(int)];
	char pid_l_[PADL_(int)]; int pid; char pid_r_[PADR_(int)];
	char flavor_l_[PADL_(int)]; int flavor; char flavor_r_[PADR_(int)];
	char arg_l_[PADL_(uint64_t)]; uint64_t arg; char arg_r_[PADR_(uint64_t)];
	char buffer_l_[PADL_(void *)]; void * buffer; char buffer_r_[PADR_(void *)];
	char buffersize_l_[PADL_(int)]; int buffersize; char buffersize_r_[PADR_(int)];
};
struct __iopolicysys_args {
	char cmd_l_[PADL_(int)]; int cmd; char cmd_r_[PADR_(int)];
	char param_l_[PADL_(struct _iopol_param_t *)]; struct _iopol_param_t * param; char param_r_[PADR_(struct _iopol_param_t *)];
};

/* From ravynOS sys/sys/sysproto.h lines 1951-2173 (kernelrpc/mach/semaphore/macx/swtch/clock/mk_timer) */
struct _kernelrpc_mach_vm_allocate_trap_args {
	char target_l_[PADL_(mach_port_name_t)]; mach_port_name_t target; char target_r_[PADR_(mach_port_name_t)];
	char address_l_[PADL_(mach_vm_offset_t *)]; mach_vm_offset_t * address; char address_r_[PADR_(mach_vm_offset_t *)];
	char size_l_[PADL_(mach_vm_size_t)]; mach_vm_size_t size; char size_r_[PADR_(mach_vm_size_t)];
	char flags_l_[PADL_(int)]; int flags; char flags_r_[PADR_(int)];
};
struct _kernelrpc_mach_vm_deallocate_trap_args {
	char target_l_[PADL_(mach_port_name_t)]; mach_port_name_t target; char target_r_[PADR_(mach_port_name_t)];
	char address_l_[PADL_(mach_vm_offset_t)]; mach_vm_offset_t address; char address_r_[PADR_(mach_vm_offset_t)];
	char size_l_[PADL_(mach_vm_size_t)]; mach_vm_size_t size; char size_r_[PADR_(mach_vm_size_t)];
};
struct _kernelrpc_mach_vm_protect_trap_args {
	char target_l_[PADL_(mach_port_name_t)]; mach_port_name_t target; char target_r_[PADR_(mach_port_name_t)];
	char address_l_[PADL_(mach_vm_offset_t)]; mach_vm_offset_t address; char address_r_[PADR_(mach_vm_offset_t)];
	char size_l_[PADL_(mach_vm_size_t)]; mach_vm_size_t size; char size_r_[PADR_(mach_vm_size_t)];
	char set_maximum_l_[PADL_(int)]; int set_maximum; char set_maximum_r_[PADR_(int)];
	char new_protection_l_[PADL_(vm_prot_t)]; vm_prot_t new_protection; char new_protection_r_[PADR_(vm_prot_t)];
};
struct _kernelrpc_mach_vm_map_trap_args {
	char target_l_[PADL_(mach_port_name_t)]; mach_port_name_t target; char target_r_[PADR_(mach_port_name_t)];
	char address_l_[PADL_(mach_vm_offset_t *)]; mach_vm_offset_t * address; char address_r_[PADR_(mach_vm_offset_t *)];
	char size_l_[PADL_(mach_vm_size_t)]; mach_vm_size_t size; char size_r_[PADR_(mach_vm_size_t)];
	char mask_l_[PADL_(mach_vm_offset_t)]; mach_vm_offset_t mask; char mask_r_[PADR_(mach_vm_offset_t)];
	char flags_l_[PADL_(int)]; int flags; char flags_r_[PADR_(int)];
	char cur_protection_l_[PADL_(vm_prot_t)]; vm_prot_t cur_protection; char cur_protection_r_[PADR_(vm_prot_t)];
};
struct _kernelrpc_mach_port_allocate_trap_args {
	char target_l_[PADL_(mach_port_name_t)]; mach_port_name_t target; char target_r_[PADR_(mach_port_name_t)];
	char right_l_[PADL_(mach_port_right_t)]; mach_port_right_t right; char right_r_[PADR_(mach_port_right_t)];
	char name_l_[PADL_(mach_port_name_t *)]; mach_port_name_t * name; char name_r_[PADR_(mach_port_name_t *)];
};
struct _kernelrpc_mach_port_destroy_trap_args {
	char target_l_[PADL_(mach_port_name_t)]; mach_port_name_t target; char target_r_[PADR_(mach_port_name_t)];
	char name_l_[PADL_(mach_port_name_t)]; mach_port_name_t name; char name_r_[PADR_(mach_port_name_t)];
};
struct _kernelrpc_mach_port_deallocate_trap_args {
	char target_l_[PADL_(mach_port_name_t)]; mach_port_name_t target; char target_r_[PADR_(mach_port_name_t)];
	char name_l_[PADL_(mach_port_name_t)]; mach_port_name_t name; char name_r_[PADR_(mach_port_name_t)];
};
struct _kernelrpc_mach_port_mod_refs_trap_args {
	char target_l_[PADL_(mach_port_name_t)]; mach_port_name_t target; char target_r_[PADR_(mach_port_name_t)];
	char name_l_[PADL_(mach_port_name_t)]; mach_port_name_t name; char name_r_[PADR_(mach_port_name_t)];
	char right_l_[PADL_(mach_port_right_t)]; mach_port_right_t right; char right_r_[PADR_(mach_port_right_t)];
	char delta_l_[PADL_(mach_port_delta_t)]; mach_port_delta_t delta; char delta_r_[PADR_(mach_port_delta_t)];
};
struct _kernelrpc_mach_port_move_member_trap_args {
	char target_l_[PADL_(mach_port_name_t)]; mach_port_name_t target; char target_r_[PADR_(mach_port_name_t)];
	char member_l_[PADL_(mach_port_name_t)]; mach_port_name_t member; char member_r_[PADR_(mach_port_name_t)];
	char after_l_[PADL_(mach_port_name_t)]; mach_port_name_t after; char after_r_[PADR_(mach_port_name_t)];
};
struct _kernelrpc_mach_port_insert_right_trap_args {
	char target_l_[PADL_(mach_port_name_t)]; mach_port_name_t target; char target_r_[PADR_(mach_port_name_t)];
	char name_l_[PADL_(mach_port_name_t)]; mach_port_name_t name; char name_r_[PADR_(mach_port_name_t)];
	char poly_l_[PADL_(mach_port_name_t)]; mach_port_name_t poly; char poly_r_[PADR_(mach_port_name_t)];
	char polyPoly_l_[PADL_(mach_msg_type_name_t)]; mach_msg_type_name_t polyPoly; char polyPoly_r_[PADR_(mach_msg_type_name_t)];
};
struct _kernelrpc_mach_port_insert_member_trap_args {
	char target_l_[PADL_(mach_port_name_t)]; mach_port_name_t target; char target_r_[PADR_(mach_port_name_t)];
	char name_l_[PADL_(mach_port_name_t)]; mach_port_name_t name; char name_r_[PADR_(mach_port_name_t)];
	char pset_l_[PADL_(mach_port_name_t)]; mach_port_name_t pset; char pset_r_[PADR_(mach_port_name_t)];
};
struct _kernelrpc_mach_port_extract_member_trap_args {
	char target_l_[PADL_(mach_port_name_t)]; mach_port_name_t target; char target_r_[PADR_(mach_port_name_t)];
	char name_l_[PADL_(mach_port_name_t)]; mach_port_name_t name; char name_r_[PADR_(mach_port_name_t)];
	char pset_l_[PADL_(mach_port_name_t)]; mach_port_name_t pset; char pset_r_[PADR_(mach_port_name_t)];
};
struct _kernelrpc_mach_port_construct_trap_args {
	char target_l_[PADL_(mach_port_name_t)]; mach_port_name_t target; char target_r_[PADR_(mach_port_name_t)];
	char options_l_[PADL_(struct mach_port_options *)]; struct mach_port_options * options; char options_r_[PADR_(struct mach_port_options *)];
	char context_l_[PADL_(uint64_t)]; uint64_t context; char context_r_[PADR_(uint64_t)];
	char name_l_[PADL_(mach_port_name_t *)]; mach_port_name_t * name; char name_r_[PADR_(mach_port_name_t *)];
};
struct _kernelrpc_mach_port_destruct_trap_args {
	char target_l_[PADL_(mach_port_name_t)]; mach_port_name_t target; char target_r_[PADR_(mach_port_name_t)];
	char name_l_[PADL_(mach_port_name_t)]; mach_port_name_t name; char name_r_[PADR_(mach_port_name_t)];
	char srdelta_l_[PADL_(mach_port_delta_t)]; mach_port_delta_t srdelta; char srdelta_r_[PADR_(mach_port_delta_t)];
	char guard_l_[PADL_(uint64_t)]; uint64_t guard; char guard_r_[PADR_(uint64_t)];
};
struct mach_reply_port_args {
	syscallarg_t dummy;
};
struct thread_self_trap_args {
	syscallarg_t dummy;
};
struct task_self_trap_args {
	syscallarg_t dummy;
};
struct host_self_trap_args {
	syscallarg_t dummy;
};
/*
 * task_get_special_port / task_set_special_port traps. Not part of the
 * Apple-imported _kernelrpc_*_trap family (Apple delivers these via
 * MIG over the task port); added here as direct syscalls because
 * freebsd-launchd-mach doesn't ship MIG. The `which` selector matches
 * TASK_BOOTSTRAP_PORT etc. from <sys/mach/task_special_ports.h>.
 */
struct task_get_special_port_trap_args {
	char target_l_[PADL_(mach_port_name_t)]; mach_port_name_t target; char target_r_[PADR_(mach_port_name_t)];
	char which_l_[PADL_(int)]; int which; char which_r_[PADR_(int)];
	char port_l_[PADL_(mach_port_name_t *)]; mach_port_name_t * port; char port_r_[PADR_(mach_port_name_t *)];
};
struct task_set_special_port_trap_args {
	char target_l_[PADL_(mach_port_name_t)]; mach_port_name_t target; char target_r_[PADR_(mach_port_name_t)];
	char which_l_[PADL_(int)]; int which; char which_r_[PADR_(int)];
	char port_l_[PADL_(mach_port_name_t)]; mach_port_name_t port; char port_r_[PADR_(mach_port_name_t)];
};
/*
 * host_set_special_port trap. Mirrors task_set_special_port: `host` is
 * the host port name (ignored — the kernel uses realhost), `which`
 * selects the slot per <sys/mach/host_special_ports.h>, `port` is the
 * send right to install (a userland mach_port_name_t).
 */
struct host_set_special_port_trap_args {
	char host_l_[PADL_(mach_port_name_t)]; mach_port_name_t host; char host_r_[PADR_(mach_port_name_t)];
	char which_l_[PADL_(int)]; int which; char which_r_[PADR_(int)];
	char port_l_[PADL_(mach_port_name_t)]; mach_port_name_t port; char port_r_[PADR_(mach_port_name_t)];
};
/*
 * Event-bell traps (Task #39 Path B). register_event_bell installs the
 * write-end fd of a userland wakeup pipe against a Mach port set;
 * unregister_event_bell tears it down. See src/mach_kmod/src/
 * mach_event_bridge.c.
 */
struct register_event_bell_trap_args {
	char port_l_[PADL_(mach_port_name_t)]; mach_port_name_t port; char port_r_[PADR_(mach_port_name_t)];
	char write_fd_l_[PADL_(int)]; int write_fd; char write_fd_r_[PADR_(int)];
};
struct unregister_event_bell_trap_args {
	char port_l_[PADL_(mach_port_name_t)]; mach_port_name_t port; char port_r_[PADR_(mach_port_name_t)];
};
/*
 * mach_wait_quiet — block until the device tree quiesces (mach.bus.busy
 * reaches 0). `timeout` is a nanosecond budget; 0 means wait
 * indefinitely. Backs Apple's IOKitWaitQuiet / IOServiceWaitQuiet. See
 * src/mach_kmod/src/mach_busystate.c.
 */
struct mach_wait_quiet_args {
	char timeout_l_[PADL_(uint64_t)]; uint64_t timeout; char timeout_r_[PADR_(uint64_t)];
};
struct mach_msg_trap_args {
	char msg_l_[PADL_(mach_msg_header_t *)]; mach_msg_header_t * msg; char msg_r_[PADR_(mach_msg_header_t *)];
	char option_l_[PADL_(mach_msg_option_t)]; mach_msg_option_t option; char option_r_[PADR_(mach_msg_option_t)];
	char send_size_l_[PADL_(mach_msg_size_t)]; mach_msg_size_t send_size; char send_size_r_[PADR_(mach_msg_size_t)];
	char rcv_size_l_[PADL_(mach_msg_size_t)]; mach_msg_size_t rcv_size; char rcv_size_r_[PADR_(mach_msg_size_t)];
	char rcv_name_l_[PADL_(mach_port_name_t)]; mach_port_name_t rcv_name; char rcv_name_r_[PADR_(mach_port_name_t)];
	char timeout_l_[PADL_(mach_msg_timeout_t)]; mach_msg_timeout_t timeout; char timeout_r_[PADR_(mach_msg_timeout_t)];
	char notify_l_[PADL_(mach_port_name_t)]; mach_port_name_t notify; char notify_r_[PADR_(mach_port_name_t)];
};
struct mach_msg_overwrite_trap_args {
	char msg_l_[PADL_(mach_msg_header_t *)]; mach_msg_header_t * msg; char msg_r_[PADR_(mach_msg_header_t *)];
	char option_l_[PADL_(mach_msg_option_t)]; mach_msg_option_t option; char option_r_[PADR_(mach_msg_option_t)];
	char send_size_l_[PADL_(mach_msg_size_t)]; mach_msg_size_t send_size; char send_size_r_[PADR_(mach_msg_size_t)];
	char rcv_size_l_[PADL_(mach_msg_size_t)]; mach_msg_size_t rcv_size; char rcv_size_r_[PADR_(mach_msg_size_t)];
	char rcv_name_l_[PADL_(mach_port_name_t)]; mach_port_name_t rcv_name; char rcv_name_r_[PADR_(mach_port_name_t)];
	char timeout_l_[PADL_(mach_msg_timeout_t)]; mach_msg_timeout_t timeout; char timeout_r_[PADR_(mach_msg_timeout_t)];
	char notify_l_[PADL_(mach_port_name_t)]; mach_port_name_t notify; char notify_r_[PADR_(mach_port_name_t)];
	char rcv_msg_l_[PADL_(mach_msg_header_t *)]; mach_msg_header_t * rcv_msg; char rcv_msg_r_[PADR_(mach_msg_header_t *)];
	char scatter_list_size_l_[PADL_(mach_msg_size_t)]; mach_msg_size_t scatter_list_size; char scatter_list_size_r_[PADR_(mach_msg_size_t)];
};
struct semaphore_signal_trap_args {
	char signal_name_l_[PADL_(mach_port_name_t)]; mach_port_name_t signal_name; char signal_name_r_[PADR_(mach_port_name_t)];
};
struct semaphore_signal_all_trap_args {
	char signal_name_l_[PADL_(mach_port_name_t)]; mach_port_name_t signal_name; char signal_name_r_[PADR_(mach_port_name_t)];
};
struct semaphore_signal_thread_trap_args {
	char signal_name_l_[PADL_(mach_port_name_t)]; mach_port_name_t signal_name; char signal_name_r_[PADR_(mach_port_name_t)];
	char thread_l_[PADL_(mach_port_name_t)]; mach_port_name_t thread; char thread_r_[PADR_(mach_port_name_t)];
};
struct semaphore_wait_trap_args {
	char wait_name_l_[PADL_(mach_port_name_t)]; mach_port_name_t wait_name; char wait_name_r_[PADR_(mach_port_name_t)];
};
struct semaphore_wait_signal_trap_args {
	char wait_name_l_[PADL_(mach_port_name_t)]; mach_port_name_t wait_name; char wait_name_r_[PADR_(mach_port_name_t)];
	char signal_name_l_[PADL_(mach_port_name_t)]; mach_port_name_t signal_name; char signal_name_r_[PADR_(mach_port_name_t)];
};
struct semaphore_timedwait_trap_args {
	char wait_name_l_[PADL_(mach_port_name_t)]; mach_port_name_t wait_name; char wait_name_r_[PADR_(mach_port_name_t)];
	char sec_l_[PADL_(unsigned int)]; unsigned int sec; char sec_r_[PADR_(unsigned int)];
	char nsec_l_[PADL_(mach_clock_res_t)]; mach_clock_res_t nsec; char nsec_r_[PADR_(mach_clock_res_t)];
};
struct semaphore_timedwait_signal_trap_args {
	char wait_name_l_[PADL_(mach_port_name_t)]; mach_port_name_t wait_name; char wait_name_r_[PADR_(mach_port_name_t)];
	char signal_name_l_[PADL_(mach_port_name_t)]; mach_port_name_t signal_name; char signal_name_r_[PADR_(mach_port_name_t)];
	char sec_l_[PADL_(unsigned int)]; unsigned int sec; char sec_r_[PADR_(unsigned int)];
	char nsec_l_[PADL_(mach_clock_res_t)]; mach_clock_res_t nsec; char nsec_r_[PADR_(mach_clock_res_t)];
};
struct _kernelrpc_mach_port_guard_trap_args {
	char target_l_[PADL_(mach_port_name_t)]; mach_port_name_t target; char target_r_[PADR_(mach_port_name_t)];
	char name_l_[PADL_(mach_port_name_t)]; mach_port_name_t name; char name_r_[PADR_(mach_port_name_t)];
	char guard_l_[PADL_(uint64_t)]; uint64_t guard; char guard_r_[PADR_(uint64_t)];
	char strict_l_[PADL_(int)]; int strict; char strict_r_[PADR_(int)];
};
struct _kernelrpc_mach_port_unguard_trap_args {
	char target_l_[PADL_(mach_port_name_t)]; mach_port_name_t target; char target_r_[PADR_(mach_port_name_t)];
	char name_l_[PADL_(mach_port_name_t)]; mach_port_name_t name; char name_r_[PADR_(mach_port_name_t)];
	char guard_l_[PADL_(uint64_t)]; uint64_t guard; char guard_r_[PADR_(uint64_t)];
};
struct task_name_for_pid_args {
	char target_tport_l_[PADL_(mach_port_name_t)]; mach_port_name_t target_tport; char target_tport_r_[PADR_(mach_port_name_t)];
	char pid_l_[PADL_(int)]; int pid; char pid_r_[PADR_(int)];
	char tn_l_[PADL_(mach_port_name_t *)]; mach_port_name_t * tn; char tn_r_[PADR_(mach_port_name_t *)];
};
struct task_for_pid_args {
	char target_tport_l_[PADL_(mach_port_name_t)]; mach_port_name_t target_tport; char target_tport_r_[PADR_(mach_port_name_t)];
	char pid_l_[PADL_(int)]; int pid; char pid_r_[PADR_(int)];
	char t_l_[PADL_(mach_port_name_t *)]; mach_port_name_t * t; char t_r_[PADR_(mach_port_name_t *)];
};
struct pid_for_task_args {
	char t_l_[PADL_(mach_port_name_t)]; mach_port_name_t t; char t_r_[PADR_(mach_port_name_t)];
	char pid_l_[PADL_(int *)]; int * pid; char pid_r_[PADR_(int *)];
};
struct macx_swapon_args {
	char name_l_[PADL_(char *)]; char * name; char name_r_[PADR_(char *)];
	char flags_l_[PADL_(int)]; int flags; char flags_r_[PADR_(int)];
	char size_l_[PADL_(int)]; int size; char size_r_[PADR_(int)];
	char priority_l_[PADL_(int)]; int priority; char priority_r_[PADR_(int)];
};
struct macx_swapoff_args {
	char name_l_[PADL_(char *)]; char * name; char name_r_[PADR_(char *)];
	char flags_l_[PADL_(int)]; int flags; char flags_r_[PADR_(int)];
};
struct macx_triggers_args {
	char hi_water_l_[PADL_(int)]; int hi_water; char hi_water_r_[PADR_(int)];
	char low_water_l_[PADL_(int)]; int low_water; char low_water_r_[PADR_(int)];
	char flags_l_[PADL_(int)]; int flags; char flags_r_[PADR_(int)];
	char alert_port_l_[PADL_(mach_port_name_t)]; mach_port_name_t alert_port; char alert_port_r_[PADR_(mach_port_name_t)];
};
struct macx_backing_store_suspend_args {
	char suspend_l_[PADL_(int)]; int suspend; char suspend_r_[PADR_(int)];
};
struct macx_backing_store_recovery_args {
	char pid_l_[PADL_(int)]; int pid; char pid_r_[PADR_(int)];
};
struct swtch_pri_args {
	char pri_l_[PADL_(int)]; int pri; char pri_r_[PADR_(int)];
};
struct swtch_args {
	syscallarg_t dummy;
};
struct thread_switch_args {
	char thread_name_l_[PADL_(mach_port_name_t)]; mach_port_name_t thread_name; char thread_name_r_[PADR_(mach_port_name_t)];
	char option_l_[PADL_(int)]; int option; char option_r_[PADR_(int)];
	char option_time_l_[PADL_(mach_msg_timeout_t)]; mach_msg_timeout_t option_time; char option_time_r_[PADR_(mach_msg_timeout_t)];
};
struct clock_sleep_trap_args {
	char clock_name_l_[PADL_(mach_port_name_t)]; mach_port_name_t clock_name; char clock_name_r_[PADR_(mach_port_name_t)];
	char sleep_type_l_[PADL_(mach_sleep_type_t)]; mach_sleep_type_t sleep_type; char sleep_type_r_[PADR_(mach_sleep_type_t)];
	char sleep_sec_l_[PADL_(int)]; int sleep_sec; char sleep_sec_r_[PADR_(int)];
	char sleep_nsec_l_[PADL_(int)]; int sleep_nsec; char sleep_nsec_r_[PADR_(int)];
	char wakeup_time_l_[PADL_(mach_timespec_t *)]; mach_timespec_t * wakeup_time; char wakeup_time_r_[PADR_(mach_timespec_t *)];
};
struct mach_timebase_info_args {
	char info_l_[PADL_(struct mach_timebase_info *)]; struct mach_timebase_info * info; char info_r_[PADR_(struct mach_timebase_info *)];
};
struct mach_wait_until_args {
	char deadline_l_[PADL_(uint64_t)]; uint64_t deadline; char deadline_r_[PADR_(uint64_t)];
};
struct mk_timer_create_args {
	syscallarg_t dummy;
};
struct mk_timer_destroy_args {
	char name_l_[PADL_(mach_port_name_t)]; mach_port_name_t name; char name_r_[PADR_(mach_port_name_t)];
};
struct mk_timer_arm_args {
	char name_l_[PADL_(mach_port_name_t)]; mach_port_name_t name; char name_r_[PADR_(mach_port_name_t)];
	char expire_time_l_[PADL_(mach_absolute_time_t)]; mach_absolute_time_t expire_time; char expire_time_r_[PADR_(mach_absolute_time_t)];
};
struct mk_timer_cancel_args {
	char name_l_[PADL_(mach_port_name_t)]; mach_port_name_t name; char name_r_[PADR_(mach_port_name_t)];
	char result_time_l_[PADL_(mach_absolute_time_t *)]; mach_absolute_time_t * result_time; char result_time_r_[PADR_(mach_absolute_time_t *)];
};

/*
 * Unset our local PADL_/PADR_ now that all the extracted Mach struct
 * defs have parsed. mach_traps.c / mach_misc.c / mach_module.c will
 * subsequently #include <sys/sysproto.h>, whose canonical PADL_/PADR_
 * definitions need a clean slate (else -Werror,-Wmacro-redefined fires
 * on the duplicate). The FreeBSD-native struct defs in sysproto.h will
 * then use the canonical padding semantics; ours used 0/0, which is
 * fine because Phase B syscalls register with NO_SYSCALL and are
 * never invoked.
 */
#undef PADL_
#undef PADR_

#endif /* _MACH_KMOD__MACH_SYSPROTO_H_ */
