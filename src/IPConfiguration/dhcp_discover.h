/*
 * dhcp_discover.h — iter 2 one-shot DHCPv4 DISCOVER/OFFER probe.
 *
 * Brings the interface up, opens a BPF descriptor on it, sends a
 * DHCPDISCOVER as a raw Ethernet frame (because the iface has no
 * IP yet), and waits for the first DHCPOFFER matching our xid.
 * Logs IPCFG-DISCOVER-OK on success / -FAIL on error / timeout —
 * that's the iter-2 boot marker.
 *
 * Full INIT → BOUND with SIOCSIFADDR + default route + the BOOTP/
 * DHCP option parser is iter 3+.
 */
#ifndef _IPCFG_DHCP_DISCOVER_H_
#define _IPCFG_DHCP_DISCOVER_H_

#include <stddef.h>	/* size_t */

/*
 * Run one DISCOVER/OFFER exchange on `ifname`. Returns 0 if an
 * OFFER was received and logged (marker emitted), non-zero
 * otherwise.
 */
int	dhcp_discover_run(const char *ifname);

/*
 * Pick the first non-loopback Ethernet interface (the QEMU guest's
 * em0 in CI). Writes the name into ifname_out (buffer is at least
 * IFNAMSIZ).  Returns 0 on success.
 */
int	dhcp_pick_interface(char *ifname_out, size_t ifname_sz);

#endif /* _IPCFG_DHCP_DISCOVER_H_ */
