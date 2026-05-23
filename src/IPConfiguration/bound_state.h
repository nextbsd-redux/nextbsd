/*
 * bound_state.h — thread-safe holder for ipconfigd's current
 * DHCPv4 BOUND lease.
 *
 * The main thread runs the DHCP exchange and writes the lease here
 * on a successful BOUND. The Mach service thread reads from here
 * when servicing RPCs (ipconfig_if_count / ipconfig_if_addr). A
 * single pthread_mutex guards the storage — the access pattern is
 * one writer + a low rate of readers, so mutex overhead is fine
 * for iter 5a. iter 5b (per-interface workers) generalizes to one
 * bound_state per interface; for now a single global suffices
 * because ipconfigd only ever DHCPs em0.
 */
#ifndef _IPCFG_BOUND_STATE_H_
#define _IPCFG_BOUND_STATE_H_

#include <net/if.h>		/* IF_NAMESIZE */
#include <stdbool.h>

#include "dhcp_packet.h"

/* Initialize the global storage. Safe to call before any writer. */
void	bound_state_init(void);

/* Mark `ifname` BOUND with `lease`. NULL ifname clears the binding. */
void	bound_state_set(const char *ifname, const struct dhcp_lease *lease);

/*
 * Snapshot whether *any* interface is bound, and (if so) the ifname.
 * `name_out` is filled iff the return is true; `name_out_sz` must
 * be at least IF_NAMESIZE.
 */
bool	bound_state_any(char *name_out, size_t name_out_sz);

/* Number of currently-bound interfaces (0 or 1 in iter 5a). */
int	bound_state_count(void);

/*
 * Look up `ifname`'s bound IPv4 address. On hit returns true and
 * writes the address into *addr_out (network byte order, packed as
 * uint32_t — what ip_address_t carries on the MIG wire).
 */
bool	bound_state_get_addr(const char *ifname, uint32_t *addr_out);

#endif /* _IPCFG_BOUND_STATE_H_ */
