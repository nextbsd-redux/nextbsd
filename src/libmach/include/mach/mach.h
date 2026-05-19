/*
 * mach/mach.h — Apple-canonical umbrella header for the Mach userland
 * API. Apple's macOS exposes a single `#include <mach/mach.h>` that
 * pulls in the entire Mach surface (mach_msg, port management, special
 * ports, host APIs, bootstrap, etc.). Apple-source consumers
 * (libxpc, launchd, configd, ...) include this one header.
 *
 * On freebsd-launchd-mach we don't have Apple's full Mach userland;
 * we provide a coordinated subset via libsystem_kernel. This header
 * is the umbrella that maps the Apple-canonical include to that subset
 * — so Apple-source code compiles unchanged.
 */
#ifndef _MACH_MACH_H_
#define _MACH_MACH_H_

#include <mach/sync_policy.h>		/* SYNC_POLICY_FIFO, ... */
#include <mach/mach_traps.h>		/* mach_reply_port, mach_task_self, ... */
#include <mach/port.h>			/* mach_port_t, mach_port_array_t, ... */
#include <mach/mach_port.h>		/* mach_port_allocate, kern_return_t */
#include <mach/message.h>		/* mach_msg, mach_msg_header_t, ... */
#include <mach/task_special_ports.h>	/* task_get_bootstrap_port, ... */
#include <mach/host_special_ports.h>	/* host_set_special_port, HOST_BOOTSTRAP_PORT */
#include <mach/machine.h>		/* cpu_type_t, cpu_subtype_t */
#include <mach/vm_map.h>			/* vm_deallocate, mach_vm_address_t */
#include <mach/mig.h>			/* mig_routine_descriptor, struct mig_subsystem */
#include <mach/mig_errors.h>		/* mig_reply_error_t, MIG_NO_REPLY */
#include <mach/mach_error.h>		/* mach_error_string */
#include <mach/error.h>			/* err_local, err_sub, err_get_code, ... */
#include <mach/task_policy.h>		/* TASK_BASE_QOS_POLICY, task_policy_set */
#include <mach/vm_param.h>		/* vm_page_size, mach_vm_*_page */
#include <mach/dispatch_kevent.h>	/* EVFILT_MACHPORT (task #39 Path B) */

#endif /* !_MACH_MACH_H_ */
