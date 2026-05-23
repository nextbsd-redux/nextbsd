/*
 * apply_lease.h — apply a DHCPv4 lease to an interface.
 *
 * Given a bound lease (yiaddr/netmask/router/dns from a DHCPACK),
 * configure the interface address via SIOCAIFADDR, install the
 * default route via the PF_ROUTE socket, and (best-effort) write
 * the DNS servers to /etc/resolv.conf. iter 3 — straight ioctl /
 * routing-message plumbing; iter 4 replaces the resolv.conf write
 * with a `State:/Network/Service/<UUID>/IPv4` + `/DNS` publish via
 * libSystemConfiguration, and adds RTM_DELETE on lease loss.
 */
#ifndef _IPCFG_APPLY_LEASE_H_
#define _IPCFG_APPLY_LEASE_H_

#include "dhcp_packet.h"

/*
 * Apply `lease` to `ifname`. Returns 0 on full success, non-zero
 * on any sub-step failure (the failing step is xlog'd). On failure
 * the partial configuration is left in place — iter 3 is an
 * "install once and stop" path; lease-loss rollback is iter 4+.
 */
int	apply_lease(const char *ifname, const struct dhcp_lease *lease);

#endif /* _IPCFG_APPLY_LEASE_H_ */
