/*
 * apply_lease_v6.h — install an IPv6 SLAAC address + default route
 * derived from a Router Advertisement.
 *
 * Iter 7a scope: one address, one default route, best-effort. No
 * lifetime tracking, no DAD wait — kernel DAD runs in the background
 * after SIOCAIFADDR_IN6 and we don't gate on it for the marker.
 */
#ifndef APPLY_LEASE_V6_H
#define APPLY_LEASE_V6_H

#include "ra_listen.h"

/*
 * Build the SLAAC unicast address from `info`'s prefix + an EUI-64
 * derived from `ifname`'s MAC, install it via SIOCAIFADDR_IN6, and
 * add a default route via info->router_lladdr. Returns 0 on success
 * (address installed; route is best-effort and logged separately).
 *
 * `out_addr` receives the computed SLAAC address for the caller's
 * SCDynamicStore publish; it is populated even if route install
 * fails downstream.
 */
int apply_ra_lease(const char *ifname, const struct ra_info *info,
    struct in6_addr *out_addr);

#endif /* APPLY_LEASE_V6_H */
