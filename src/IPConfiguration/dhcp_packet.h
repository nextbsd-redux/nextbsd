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

#endif /* _IPCFG_DHCP_PACKET_H_ */
