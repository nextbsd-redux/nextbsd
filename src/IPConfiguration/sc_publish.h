/*
 * sc_publish.h — SCDynamicStore publish glue for ipconfigd.
 *
 * Wraps an SCDynamicStoreRef so the DHCPv4 state machine can publish
 * a bound lease under State:/Network/Service/<UUID>/IPv4 — Apple's
 * canonical key for the IPConfiguration -> SystemConfiguration
 * handoff. A SystemConfiguration client (SCNetworkService observer,
 * ipconfig(8), an NSD plugin) sees the same dictionary shape it
 * expects on macOS: Addresses, SubnetMasks, Router, InterfaceName,
 * ServerIdentifier. DNS goes under .../DNS as ServerAddresses.
 *
 * The handle is opaque so the CoreFoundation / SCDynamicStore.h pull
 * stays inside sc_publish.c; ipconfigd.c / lease_loop.c only see
 * plain-C struct dhcp_lease and char *ifname.
 */
#ifndef _IPCFG_SC_PUBLISH_H_
#define _IPCFG_SC_PUBLISH_H_

#include "dhcp_packet.h"

#include <netinet/in.h>
#include <stdint.h>

struct sc_publish;

/*
 * Open a session with configd. session_name is the human-readable
 * label configd uses to identify this publisher (shows up in its
 * session table). Returns NULL on failure — the daemon should keep
 * running (CI marker IPCFG-STORE-OK won't fire, but DHCP still
 * works at the kernel level via apply_lease).
 */
struct sc_publish	*sc_publish_open(const char *session_name);

/*
 * Publish lease state for `ifname`. Sets two keys:
 *   State:/Network/Service/<UUID>/IPv4
 *   State:/Network/Service/<UUID>/DNS   (only if lease has DNS)
 * The <UUID> is derived deterministically from the interface MAC so
 * the keys are stable across daemon restarts. Returns 0 on success.
 */
int	sc_publish_ipv4(struct sc_publish *p, const char *ifname,
	    const struct dhcp_lease *lease);

/*
 * Publish iter 7a SLAAC state under State:/Network/Service/<UUID>/IPv6.
 * Mirrors Apple's IPv6 dict shape: Addresses (array of strings),
 * PrefixLength (array of numbers), Router (link-local address with
 * %ifname zone), InterfaceName, Flags=0 (SLAAC). Returns 0 on success.
 *
 * Single-address publish only; iter 7a doesn't track multiple PIOs.
 */
int	sc_publish_ipv6(struct sc_publish *p, const char *ifname,
	    const struct in6_addr *addr, uint8_t prefix_len,
	    const struct in6_addr *router_lladdr);

/*
 * Publish DHCP state under State:/Network/Service/<UUID>/DHCP. Mirrors
 * Apple's IPMonitor DHCP-options dict shape: per-option entries keyed
 * "Option_<N>" (CFData on Apple; CFString for printable options like
 * 12 here so consumers — hostnamed iter 3, mDNSResponder — can read
 * directly without re-decoding), plus LeaseStartTime (CFNumber,
 * CLOCK_MONOTONIC seconds at bind). Issue #88.
 *
 * The /DHCP key is always set on BOUND (so observers can subscribe to
 * its appearance), but Option_12 entries only land when the lease
 * actually carried a server-supplied host name. Returns 0 on success.
 */
int	sc_publish_dhcp(struct sc_publish *p, const char *ifname,
	    const struct dhcp_lease *lease);

/*
 * Remove the previously-published keys for `ifname`. Called on
 * lease loss (REBINDING also fails) so observers see the service
 * transition to "no v4".
 */
int	sc_publish_remove(struct sc_publish *p, const char *ifname);

/* Close the session. Safe to pass NULL. */
void	sc_publish_close(struct sc_publish *p);

#endif /* _IPCFG_SC_PUBLISH_H_ */
