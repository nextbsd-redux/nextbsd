/*
 * mach/mach_types.h — userland Mach object-port type vocabulary.
 *
 * In userland, every Mach "object" (a task, thread, host, IPC space,
 * ...) is represented by a mach_port_t naming a send right to that
 * object. This header is the typedef set for those names. Apple's
 * real <mach/mach_types.h> is large and kernel-coupled (ravynOS's
 * sys/sys/mach/mach_types.h is 368 lines with MALLOC_DECLARE and
 * kernel locking includes); we provide just the flat userland
 * typedefs the MIG-generated stubs and launchd-842 actually use.
 *
 * Pulled in by the MIG output (job.h etc.) because mach_types.defs
 * does `import <sys/mach/mach_types.h>` — rewritten by us to
 * <mach/mach_types.h>.
 */
#ifndef _MACH_MACH_TYPES_H_
#define _MACH_MACH_TYPES_H_

#include <mach/port.h>		/* mach_port_t, mach_port_array_t */
#include <mach/message.h>	/* mach_msg_* */
#include <mach/machine.h>	/* cpu_type_t */
#include <mach/vm_map.h>	/* vm_map_t, vm_*_t */
#include <mach/kern_return.h>	/* kern_return_t */
#include <mach/std_types.h>	/* natural_t, integer_t */

/* Task / thread object ports. */
typedef mach_port_t	task_t;
typedef mach_port_t	task_name_t;
typedef mach_port_t	task_inspect_t;
typedef mach_port_t	task_suspension_token_t;
typedef mach_port_t	thread_t;
typedef mach_port_t	thread_act_t;
typedef mach_port_t	thread_inspect_t;
typedef task_t		*task_array_t;
typedef thread_t	*thread_act_array_t;

/* Host object ports. */
typedef mach_port_t	host_t;
typedef mach_port_t	host_priv_t;
typedef mach_port_t	host_security_t;
typedef host_t		host_name_t;
typedef host_t		host_name_port_t;

/* IPC / processor / misc object ports. */
typedef mach_port_t	ipc_space_t;
typedef mach_port_t	processor_t;
typedef mach_port_t	processor_set_t;
typedef mach_port_t	processor_set_control_t;
typedef processor_set_t	processor_set_name_t;
typedef mach_port_t	*processor_array_t;
typedef mach_port_t	semaphore_t;
typedef mach_port_t	lock_set_t;
typedef mach_port_t	ledger_t;
typedef mach_port_t	alarm_t;
typedef mach_port_t	clock_serv_t;
typedef mach_port_t	clock_ctrl_t;
typedef mach_port_t	io_master_t;
typedef mach_port_t	coalition_t;
typedef mach_port_t	exception_handler_t;
typedef exception_handler_t *exception_handler_array_t;

/* Voucher subsystem object port. Apple's voucher API (mach_voucher,
 * voucher_recipe, etc.) is not implemented here; the typedef exists
 * so Apple-source consumers (libdispatch/os/voucher_private.h,
 * launchd's voucher tracking) parse. */
typedef mach_port_t	mach_voucher_t;
typedef mach_port_t	ipc_voucher_t;
#ifndef MACH_VOUCHER_NULL
#define MACH_VOUCHER_NULL ((mach_voucher_t)0)
#endif

/* Polymorphic / convenience names. */
typedef mach_port_t	mach_port_poly_t;
typedef integer_t	ledger_item_t;
typedef integer_t	ledger_amount_t;

#endif /* !_MACH_MACH_TYPES_H_ */
