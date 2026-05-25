/*
 * dhcp_packet.h — DHCPv4 wire format (RFC 2131 / 2132).
 *
 * Self-contained, sized to what iter 2 needs (DISCOVER/OFFER). The
 * struct layout mirrors Apple's bootp/bootplib/dhcp.h — same field
 * names — so when later iters pull in dhcp_options.c verbatim the
 * struct names line up. Constants are RFC, not Apple-private.
 */
#ifndef _IPCFG_DHCP_PACKET_H_
#define _IPCFG_DHCP_PACKET_H_

#include <sys/types.h>
#include <netinet/in.h>
#include <stdint.h>

#define DHCP_MAGIC_COOKIE	{ 99, 130, 83, 99 }	/* RFC 1497 */

/* DHCP message-type option values (option 53). */
#define DHCP_MTYPE_DISCOVER	1
#define DHCP_MTYPE_OFFER	2
#define DHCP_MTYPE_REQUEST	3
#define DHCP_MTYPE_DECLINE	4
#define DHCP_MTYPE_ACK		5
#define DHCP_MTYPE_NAK		6
#define DHCP_MTYPE_RELEASE	7
#define DHCP_MTYPE_INFORM	8

/* BOOTP opcodes. */
#define BOOTREQUEST		1
#define BOOTREPLY		2

#define HTYPE_ETHERNET		1

/* DHCP option codes (subset for iter 2). */
#define DHCP_OPT_PAD		  0
#define DHCP_OPT_SUBNET_MASK	  1
#define DHCP_OPT_ROUTER		  3
#define DHCP_OPT_DNS_SERVER	  6
#define DHCP_OPT_HOST_NAME	 12
#define DHCP_OPT_REQUESTED_IP	 50
#define DHCP_OPT_LEASE_TIME	 51
#define DHCP_OPT_MESSAGE_TYPE	 53
#define DHCP_OPT_SERVER_ID	 54
#define DHCP_OPT_PARAM_REQUEST	 55
#define DHCP_OPT_CLIENT_ID	 61
#define DHCP_OPT_END		255

#define DHCP_FLAG_BROADCAST	0x8000

/*
 * The DHCP/BOOTP header. Apple names the fields dp_*. Sized 236
 * bytes; options follow.
 */
struct dhcp {
	uint8_t		dp_op;
	uint8_t		dp_htype;
	uint8_t		dp_hlen;
	uint8_t		dp_hops;
	uint32_t	dp_xid;
	uint16_t	dp_secs;
	uint16_t	dp_flags;
	struct in_addr	dp_ciaddr;
	struct in_addr	dp_yiaddr;
	struct in_addr	dp_siaddr;
	struct in_addr	dp_giaddr;
	uint8_t		dp_chaddr[16];
	uint8_t		dp_sname[64];
	uint8_t		dp_file[128];
	/* 4-byte magic cookie + variable options follow as packed
	 * bytes; we treat them as an opaque tail. */
};

#define DHCP_PACKET_MIN		576	/* RFC 2131 minimum 576 octets */

/*
 * iter-3 lease carrier — the subset of an ACK we care about for
 * apply_lease. Extra options (broadcast addr, NTP, etc.) can grow
 * this struct in later iters without changing existing callers.
 */
#define DHCP_LEASE_MAX_DNS	4

/* DHCP option 12 max length per RFC 2132 §3.14. +1 for terminating NUL
 * so the buffer is always safe to pass to printf / strncpy. */
#define DHCP_LEASE_MAX_HOSTNAME	256

struct dhcp_lease {
	struct in_addr	addr;		/* yiaddr from the ACK */
	struct in_addr	netmask;	/* option 1 (default 255.0.0.0) */
	struct in_addr	router;		/* option 3 (first entry) */
	struct in_addr	server;		/* option 54 (server identifier) */
	uint32_t	lease_time;	/* option 51 (seconds) */
	struct in_addr	dns[DHCP_LEASE_MAX_DNS];
	unsigned	dns_count;
	/* Option 12 host name — server-supplied client name. Issue #88;
	 * hostnamed iter 3 reads State:/Network/Service/<UUID>/DHCP/Option_12
	 * to fold this into its precedence chain. host_name_len = 0 means
	 * the option was absent from the lease. */
	char		host_name[DHCP_LEASE_MAX_HOSTNAME];
	unsigned	host_name_len;
};

#endif /* _IPCFG_DHCP_PACKET_H_ */
