/*
 * hwreg_subscribe.h — IPConfiguration iter 9 hwregd pub/sub client.
 *
 * Subscribe to org.freebsd.hwregd via the raw HWREG_MSG_SUBSCRIBE
 * wire protocol (same shape DiskArbitration uses) so ipconfigd can
 * react to NICs that hwregd autoloads after the boot-settle window.
 * The path that motivates this: a NIC whose driver is neither built
 * into the kernel nor preloaded in /boot/loader.conf isn't visible
 * to getifaddrs at ipconfigd's startup; hwregd's iter "drain the
 * deferred backlog" PR (#60) kldload(2)s the driver ~60s into boot,
 * the kernel posts a +attach devctl line, hwregd republishes it on
 * its pub/sub bus, and this subscriber kicks off DHCP on the new
 * interface — closing the "first boot on autoloaded NICs" gap.
 *
 * iter 9 deliberately stays on the raw pub/sub (no MIG hwreg_watch
 * + nvlist criteria) because the subscriber doesn't gain anything
 * by filtering at hwregd: it has to re-run getifaddrs() to confirm
 * the interface is an Ethernet anyway. Same call shape DA's iter 2
 * uses (see src/DiskArbitration/hwreg_subscribe.{c,h}).
 */
#ifndef IPCONFIGURATION_HWREG_SUBSCRIBE_H
#define IPCONFIGURATION_HWREG_SUBSCRIBE_H

#include <stdint.h>

/*
 * Callback invoked from the subscriber thread when hwregd posts a
 * `+` attach event whose dev=<ifname> looks plausibly like a NIC
 * (getifaddrs filtering happens inside the callback, not here). The
 * callback owns whether to actually run DHCP — typically it consults
 * bound_state to skip already-bound interfaces, then runs the DHCP
 * state machine + lease loop. The callback BLOCKS the subscriber
 * thread until it returns; once it enters lease_loop_run no further
 * events are processed (single-NIC focus — multi-NIC fan-out is a
 * later iter). `lease_cap_secs` is forwarded from main.
 */
typedef void (*hwreg_subscribe_attach_cb)(const char *ifname,
    uint32_t lease_cap_secs);

/*
 * Start the hwregd subscription. Emits IPCFG-AUTOLOAD-SUB-OK on a
 * successful ack-receive, IPCFG-AUTOLOAD-SUB-FAIL otherwise. Returns
 * 0 on success (subscription established + receive thread running),
 * -1 on any error (logged inline). Non-fatal — the daemon stays up
 * and keeps its Mach service registered, just without auto-react.
 */
int	hwreg_subscribe_start(hwreg_subscribe_attach_cb cb,
	    uint32_t lease_cap_secs);

#endif /* IPCONFIGURATION_HWREG_SUBSCRIBE_H */
