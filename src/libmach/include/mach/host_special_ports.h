/*
 * mach/host_special_ports.h — userland API for host-level special
 * ports. Apple's macOS exposes these via host_set_special_port (over
 * MIG to the host port). freebsd-launchd-mach exposes
 * host_set_special_port as a direct mach.ko-registered syscall — no
 * MIG involved.
 *
 * Current scope: only HOST_BOOTSTRAP_PORT is settable from userland.
 * The bootstrap server calls
 *
 *   host_set_special_port(mach_host_self(), HOST_BOOTSTRAP_PORT, port);
 *
 * once at startup; thereafter task_get_bootstrap_port() falls back to
 * this slot when the calling task's per-task itk_bootstrap is null,
 * giving every process a way to discover the bootstrap server's port
 * without needing fork-inheritance from a launchd PID 1.
 *
 * Returns KERN_INVALID_ARGUMENT for other `which` values; returns
 * KERN_RESOURCE_SHORTAGE if mach.ko isn't loaded.
 */
#ifndef _MACH_HOST_SPECIAL_PORTS_H_
#define _MACH_HOST_SPECIAL_PORTS_H_

#include <mach/mach_traps.h>		/* mach_port_name_t */
#include <mach/mach_port.h>		/* kern_return_t */
#include <mach/message.h>		/* mach_port_t */
#include <mach/mach_types.h>		/* host_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Slot numbers (matches kernel-side <sys/mach/host_special_ports.h>). */
#define HOST_PORT			1
#define HOST_PRIV_PORT			2
#define HOST_IO_MASTER_PORT		3
#define HOST_BOOTSTRAP_PORT		13	/* freebsd-launchd-mach */
#define HOST_KEXTD_PORT			15	/* kextd load-request port (K3b, #216) */

/*
 * Upper bound on a host-special-port `which` value. launchd uses it
 * to validate ports it's asked to set as host-special. Apple's value
 * is the kernel-internal high-water mark; we leave headroom past
 * HOST_BOOTSTRAP_PORT for any future slots.
 */
#define HOST_MAX_SPECIAL_KERNEL_PORT	16
#define HOST_MAX_SPECIAL_PORT		31

kern_return_t host_set_special_port(mach_port_name_t host,
    int which, mach_port_t port);

/*
 * host_set_UNDServer() — set the User Notification Daemon port.
 * macOS-specific; stub on this port.
 */
kern_return_t host_set_UNDServer(host_t host, mach_port_t port);

#ifdef __cplusplus
}
#endif

#endif /* !_MACH_HOST_SPECIAL_PORTS_H_ */
