/*
 * dhcp_discover.h — DHCPv4 client front door.
 *
 * Despite the historical name (iter 2 was just DISCOVER/OFFER),
 * this module now runs the full RFC 2131 INIT → SELECTING →
 * REQUESTING → BOUND state machine: brings the interface up,
 * opens a BPF descriptor, sends DHCPDISCOVER (retransmitted with
 * the standard 4/8/16s backoff + jitter), parses the first
 * DHCPOFFER it sees, sends DHCPREQUEST, parses the DHCPACK, and
 * hands the caller a struct dhcp_lease.
 *
 * Long BPF read waits are signal-aware: a SIGTERM/SIGHUP arriving
 * during a wait short-circuits the loop (the daemon's global
 * `got_term` flag is checked between reads + on EINTR).
 *
 * Lease persistence, T1/T2 renewal, RENEWING/REBINDING — iter 4.
 */
#ifndef _IPCFG_DHCP_DISCOVER_H_
#define _IPCFG_DHCP_DISCOVER_H_

#include <signal.h>	/* sig_atomic_t */
#include <stddef.h>	/* size_t */

#include "dhcp_packet.h"

/*
 * The daemon's signal-asserted shutdown flag (defined in
 * ipconfigd.c). dhcp_discover poll loops check it between BPF
 * reads so a SIGTERM during the (potentially multi-second) DHCP
 * retransmit window short-circuits the wait — fixes iter 2's
 * 10-second deafness.
 */
extern volatile sig_atomic_t got_term;

/*
 * Run one INIT → BOUND exchange on `ifname`. On success returns 0
 * and fills `*out` with the bound lease. The caller passes the
 * lease to apply_lease() to actually configure the interface.
 *
 * Logs IPCFG-BOUND-FAIL on its own line and returns non-zero on
 * any error / timeout.
 */
int	dhcp_lease_acquire(const char *ifname, struct dhcp_lease *out);

/*
 * Renew a bound lease — send a DHCPREQUEST in BOUND/RENEWING form
 * (ciaddr=current addr, no opt 50/54, broadcast flag set so SLIRP
 * and other simple servers accept it without ARP-resolving the
 * client first), wait for the DHCPACK, fill *new_lease.
 *
 * iter 4 keeps RENEWING and REBINDING on the same broadcast-BPF
 * path; RFC 2131 §4.3.2 says RENEWING SHOULD unicast but servers
 * MAY (and most do) accept the broadcast form. Splitting unicast
 * out is iter 5+ work alongside the threading rework.
 *
 * Returns 0 on success, -1 on timeout / shutdown / fatal error.
 */
int	dhcp_renew(const char *ifname,
	    const struct dhcp_lease *existing,
	    struct dhcp_lease *new_lease);

/*
 * Pick the first non-loopback Ethernet interface (the QEMU guest's
 * em0 in CI). Writes the name into ifname_out (buffer is at least
 * IFNAMSIZ).  Returns 0 on success.
 */
int	dhcp_pick_interface(char *ifname_out, size_t ifname_sz);

#endif /* _IPCFG_DHCP_DISCOVER_H_ */
