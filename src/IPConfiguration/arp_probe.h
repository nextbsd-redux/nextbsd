/*
 * arp_probe.h — RFC 5227 IPv4 ARP probe + announce.
 *
 * Iter 6 of IPConfiguration: address-conflict detection on the
 * DHCPOFFER and gratuitous-ARP announcement after the lease is
 * applied.
 *
 * Probe path (called between OFFER and REQUEST): send 3 ARP
 * requests with sender IP = 0.0.0.0, target IP = offered addr.
 * Any reply, OR any ARP request whose sender IP is the probed
 * address, is treated as a conflict and the caller should
 * DHCPDECLINE the offer (RFC 5227 §2.1.1).
 *
 * Announce path (called from apply_lease after SIOCAIFADDR):
 * send 2 gratuitous ARP requests with sender IP = target IP =
 * our IP, updating peer ARP caches (RFC 5227 §2.3).
 *
 * Not vendored from Apple — bootp/bootplib/arp.{c,h} is route-
 * socket ARP cache plumbing, and IPConfiguration.bproj/
 * arp_session.c (the actual RFC 5227 implementation) is a
 * ~2500-LOC event-channel state machine; iter 6's MVP is small
 * synchronous BPF send/recv that reuses dhcp_discover's
 * pattern.
 */
#ifndef _IPCFG_ARP_PROBE_H_
#define _IPCFG_ARP_PROBE_H_

#include <netinet/in.h>
#include <stdint.h>

/*
 * Probe `target` on `ifname` per RFC 5227 §2.1.1.
 *
 *   0  no conflict (safe to claim the address)
 *   1  conflict detected (caller should DHCPDECLINE)
 *  -1  fatal error (BPF open / send failure); caller treats as
 *      no-conflict so a transient probe issue doesn't deadlock
 *      the DHCP flow
 */
int	arp_probe(const char *ifname, struct in_addr target);

/*
 * Announce ownership of `our_ip` on `ifname` per RFC 5227 §2.3.
 *
 *   0  on full success
 *  -1  on any send failure (logged; caller continues — the
 *      address is already on the interface, the announce is
 *      advisory)
 */
int	arp_announce(const char *ifname, struct in_addr our_ip);

#endif /* _IPCFG_ARP_PROBE_H_ */
