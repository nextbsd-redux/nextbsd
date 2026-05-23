/*
 * dhcp_discover.c — iter 2 one-shot DHCPv4 DISCOVER/OFFER probe.
 *
 * The minimum DHCPv4 logic to prove the daemon can fly packets:
 * pick an interface, bring it up, open a BPF descriptor, send a
 * raw Ethernet/IP/UDP DHCPDISCOVER (the interface has no IP yet
 * so plain UDP sockets can't address the DHCP server), read raw
 * frames until a DHCPOFFER matching our xid arrives.
 *
 * NOT vendored from Apple's IPConfiguration.bproj. The Apple
 * bootp/bootplib/{bpflib,dhcp_options}.c source is the long-term
 * destination; this iter writes ~250 LOC of clean BSD-licensed
 * code to land the marker fast. iter 3 starts pulling in Apple's
 * option parser when REQUEST/ACK needs it.
 *
 * QEMU SLIRP user-network (the CI guest's only NIC) runs an
 * internal DHCP server at 10.0.2.2 offering 10.0.2.15 — the
 * deterministic test target.
 */
#include "dhcp_discover.h"
#include "dhcp_packet.h"

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <net/bpf.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#include <arpa/inet.h>

#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DHCP_CLIENT_PORT	68
#define DHCP_SERVER_PORT	67

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "ipconfigd[dhcp] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/* RFC 1071 one's-complement Internet checksum. */
static uint16_t
in_cksum(const void *buf, size_t len)
{
	const uint8_t *p = buf;
	uint32_t sum = 0;

	while (len > 1) {
		sum += (uint32_t)((p[0] << 8) | p[1]);
		p += 2;
		len -= 2;
	}
	if (len == 1)
		sum += (uint32_t)(p[0] << 8);
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return ((uint16_t)~sum);
}

int
dhcp_pick_interface(char *ifname_out, size_t ifname_sz)
{
	struct ifaddrs *ifa, *p;
	int found = 0;

	if (getifaddrs(&ifa) != 0)
		return (-1);
	for (p = ifa; p != NULL; p = p->ifa_next) {
		const struct sockaddr_dl *dl;

		if (p->ifa_addr == NULL ||
		    p->ifa_addr->sa_family != AF_LINK)
			continue;
		if ((p->ifa_flags & IFF_LOOPBACK) != 0)
			continue;
		dl = (const struct sockaddr_dl *)(void *)p->ifa_addr;
		/* IFT_ETHER = 6, the type for ordinary Ethernet. */
		if (dl->sdl_type != 6)
			continue;
		(void)strlcpy(ifname_out, p->ifa_name, ifname_sz);
		found = 1;
		break;
	}
	freeifaddrs(ifa);
	return (found ? 0 : -1);
}

/*
 * Copy the AF_LINK MAC for `ifname` into mac[6]. Returns 0 on
 * success.
 */
static int
get_interface_mac(const char *ifname, uint8_t mac[6])
{
	struct ifaddrs *ifa, *p;
	int ok = -1;

	if (getifaddrs(&ifa) != 0)
		return (-1);
	for (p = ifa; p != NULL; p = p->ifa_next) {
		const struct sockaddr_dl *dl;

		if (p->ifa_addr == NULL ||
		    p->ifa_addr->sa_family != AF_LINK)
			continue;
		if (strcmp(p->ifa_name, ifname) != 0)
			continue;
		dl = (const struct sockaddr_dl *)(void *)p->ifa_addr;
		if (dl->sdl_alen != 6)
			break;
		(void)memcpy(mac, LLADDR(dl), 6);
		ok = 0;
		break;
	}
	freeifaddrs(ifa);
	return (ok);
}

/* OR IFF_UP into ifname's flags via a configuration socket. */
static int
bring_interface_up(const char *ifname)
{
	struct ifreq ifr;
	int sock, rc;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		return (-1);
	(void)memset(&ifr, 0, sizeof(ifr));
	(void)strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	if (ioctl(sock, SIOCGIFFLAGS, &ifr) != 0) {
		(void)close(sock);
		return (-1);
	}
	if ((ifr.ifr_flags & IFF_UP) == 0) {
		ifr.ifr_flags |= IFF_UP;
		rc = ioctl(sock, SIOCSIFFLAGS, &ifr);
	} else {
		rc = 0;
	}
	(void)close(sock);
	return (rc);
}

/*
 * Open /dev/bpfN, BIOCSETIF to ifname, set immediate mode + the
 * hdrcmplt flag so our prebuilt Ethernet header is sent verbatim.
 */
static int
bpf_open(const char *ifname)
{
	struct ifreq ifr;
	int fd = -1, i;
	u_int one = 1;
	struct timeval tv;

	for (i = 0; i < 256; i++) {
		char path[32];

		(void)snprintf(path, sizeof(path), "/dev/bpf%d", i);
		fd = open(path, O_RDWR);
		if (fd >= 0)
			break;
		if (errno != EBUSY)
			break;
	}
	if (fd < 0)
		return (-1);

	(void)memset(&ifr, 0, sizeof(ifr));
	(void)strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	if (ioctl(fd, BIOCSETIF, &ifr) != 0) {
		(void)close(fd);
		return (-1);
	}
	/* immediate = wake on each packet. */
	(void)ioctl(fd, BIOCIMMEDIATE, &one);
	/* hdrcmplt = 1 — we provide the full Ethernet header (incl.
	 * the src MAC); the kernel must NOT rewrite it. */
	(void)ioctl(fd, BIOCSHDRCMPLT, &one);
	/* Read timeout: 5s. Receive blocks at most this long. */
	tv.tv_sec = 5;
	tv.tv_usec = 0;
	(void)ioctl(fd, BIOCSRTIMEOUT, &tv);
	return (fd);
}

/*
 * Build the iter-2 DISCOVER frame into `buf` (must be at least
 * 1024 bytes). Returns the total frame length.
 *
 * Layout:
 *   [0..13]                   Ethernet (dst=broadcast, src=mac, type=0x0800)
 *   [14..33]                  IPv4 (src=0.0.0.0, dst=255.255.255.255, proto=17)
 *   [34..41]                  UDP (sport=68, dport=67)
 *   [42..277]                 BOOTP/DHCP fixed-form (236 bytes)
 *   [278..281]                magic cookie
 *   [282..]                   options (msg-type DISCOVER, client-id, param-req, end, pad)
 *
 * Pad to >= DHCP_PACKET_MIN-overhead so router/firewall hardware
 * with minimum-payload assumptions doesn't drop the frame.
 */
static size_t
build_discover(uint8_t *buf, const uint8_t mac[6], uint32_t xid)
{
	struct ether_header *eh;
	struct ip *ip;
	struct udphdr *udp;
	struct dhcp *dh;
	const uint8_t cookie[4] = DHCP_MAGIC_COOKIE;
	uint8_t *opt;
	size_t udp_len, ip_len, frame_len;
	uint8_t pseudo[12];

	(void)memset(buf, 0, 1024);

	eh = (struct ether_header *)(void *)buf;
	(void)memset(eh->ether_dhost, 0xff, 6);
	(void)memcpy(eh->ether_shost, mac, 6);
	eh->ether_type = htons(ETHERTYPE_IP);

	ip = (struct ip *)(void *)(buf + sizeof(*eh));
	ip->ip_v = 4;
	ip->ip_hl = 5;
	ip->ip_tos = 0;
	ip->ip_id = htons(0);
	ip->ip_off = 0;
	ip->ip_ttl = 64;
	ip->ip_p = IPPROTO_UDP;
	ip->ip_sum = 0;
	ip->ip_src.s_addr = htonl(INADDR_ANY);
	ip->ip_dst.s_addr = htonl(INADDR_BROADCAST);

	udp = (struct udphdr *)(void *)(buf + sizeof(*eh) + sizeof(*ip));
	udp->uh_sport = htons(DHCP_CLIENT_PORT);
	udp->uh_dport = htons(DHCP_SERVER_PORT);
	udp->uh_sum = 0;

	dh = (struct dhcp *)(void *)(buf + sizeof(*eh) + sizeof(*ip) +
	    sizeof(*udp));
	dh->dp_op = BOOTREQUEST;
	dh->dp_htype = HTYPE_ETHERNET;
	dh->dp_hlen = 6;
	dh->dp_xid = htonl(xid);
	dh->dp_flags = htons(DHCP_FLAG_BROADCAST);
	(void)memcpy(dh->dp_chaddr, mac, 6);

	opt = (uint8_t *)dh + sizeof(*dh);
	(void)memcpy(opt, cookie, 4);
	opt += 4;
	/* option 53: DHCP message type = DISCOVER */
	*opt++ = DHCP_OPT_MESSAGE_TYPE;
	*opt++ = 1;
	*opt++ = DHCP_MTYPE_DISCOVER;
	/* option 61: client id = htype 01 + mac */
	*opt++ = DHCP_OPT_CLIENT_ID;
	*opt++ = 7;
	*opt++ = HTYPE_ETHERNET;
	(void)memcpy(opt, mac, 6);
	opt += 6;
	/* option 55: parameter request list (subnet, router, DNS,
	 * lease, server-id). iter 2 doesn't consume these but
	 * sending the list shapes the OFFER. */
	*opt++ = DHCP_OPT_PARAM_REQUEST;
	*opt++ = 5;
	*opt++ = DHCP_OPT_SUBNET_MASK;
	*opt++ = DHCP_OPT_ROUTER;
	*opt++ = DHCP_OPT_DNS_SERVER;
	*opt++ = DHCP_OPT_LEASE_TIME;
	*opt++ = DHCP_OPT_SERVER_ID;
	*opt++ = DHCP_OPT_END;

	/* Pad up to the RFC 2131 minimum frame (576 IP bytes). */
	frame_len = (size_t)(opt - buf);
	while (frame_len < sizeof(*eh) + DHCP_PACKET_MIN) {
		buf[frame_len++] = DHCP_OPT_PAD;
	}

	udp_len = frame_len - sizeof(*eh) - sizeof(*ip);
	ip_len = frame_len - sizeof(*eh);
	ip->ip_len = htons((u_short)ip_len);
	udp->uh_ulen = htons((u_short)udp_len);

	/* IP header checksum (header only). Recompute after setting
	 * ip_len. */
	ip->ip_sum = 0;
	ip->ip_sum = htons(in_cksum(ip, sizeof(*ip)));

	/* UDP checksum: pseudo-header(12) + UDP header + payload.
	 * SLIRP requires a valid UDP checksum on broadcast DHCP. */
	(void)memset(pseudo, 0, sizeof(pseudo));
	(void)memcpy(pseudo, &ip->ip_src.s_addr, 4);
	(void)memcpy(pseudo + 4, &ip->ip_dst.s_addr, 4);
	pseudo[8] = 0;
	pseudo[9] = IPPROTO_UDP;
	pseudo[10] = (uint8_t)(udp_len >> 8);
	pseudo[11] = (uint8_t)(udp_len & 0xff);
	{
		/* Run the checksum over a temp buffer concatenating
		 * the pseudo-header with the UDP header+payload. */
		uint8_t tmp[2048];
		size_t n;

		if (udp_len > sizeof(tmp) - sizeof(pseudo)) {
			/* should not happen for iter-2 sizes */
			return (frame_len);
		}
		(void)memcpy(tmp, pseudo, sizeof(pseudo));
		(void)memcpy(tmp + sizeof(pseudo), udp, udp_len);
		n = sizeof(pseudo) + udp_len;
		udp->uh_sum = htons(in_cksum(tmp, n));
		if (udp->uh_sum == 0)
			udp->uh_sum = 0xffff;	/* RFC 768: 0 means "no cksum" */
	}

	return (frame_len);
}

/*
 * Scan DHCP options for the message type (53). Returns the value,
 * or 0 if not found. opts_len is the trailing space available
 * past the magic cookie.
 */
static uint8_t
dhcp_msgtype(const uint8_t *opts, size_t opts_len)
{
	size_t i = 4;	/* skip the magic cookie */

	while (i + 1 < opts_len) {
		uint8_t code = opts[i];
		uint8_t len;

		if (code == DHCP_OPT_PAD) {
			i++;
			continue;
		}
		if (code == DHCP_OPT_END)
			break;
		len = opts[i + 1];
		if (i + 2 + len > opts_len)
			break;
		if (code == DHCP_OPT_MESSAGE_TYPE && len == 1)
			return (opts[i + 2]);
		i += 2 + len;
	}
	return (0);
}

/*
 * Fetch the value of option `code` from the options blob (after
 * the cookie). Returns a pointer + length, or NULL on absence.
 */
static const uint8_t *
dhcp_option(const uint8_t *opts, size_t opts_len, uint8_t code,
    uint8_t *len_out)
{
	size_t i = 4;

	while (i + 1 < opts_len) {
		uint8_t c = opts[i];
		uint8_t l;

		if (c == DHCP_OPT_PAD) {
			i++;
			continue;
		}
		if (c == DHCP_OPT_END)
			break;
		l = opts[i + 1];
		if (i + 2 + l > opts_len)
			break;
		if (c == code) {
			*len_out = l;
			return (&opts[i + 2]);
		}
		i += 2 + l;
	}
	return (NULL);
}

/*
 * Parse one BPF-delivered frame. Returns 1 + sets yiaddr_out /
 * server_out if it is a DHCPOFFER with our xid; 0 otherwise.
 */
static int
parse_offer(const uint8_t *frame, size_t frame_len, uint32_t want_xid,
    struct in_addr *yiaddr_out, struct in_addr *server_out)
{
	const struct ether_header *eh;
	const struct ip *ip;
	const struct udphdr *udp;
	const struct dhcp *dh;
	const uint8_t *opts;
	const uint8_t cookie[4] = DHCP_MAGIC_COOKIE;
	const uint8_t *v;
	size_t opts_len;
	uint8_t l = 0;

	if (frame_len < sizeof(*eh) + sizeof(*ip) + sizeof(*udp) +
	    sizeof(*dh) + 4)
		return (0);
	eh = (const struct ether_header *)(const void *)frame;
	if (ntohs(eh->ether_type) != ETHERTYPE_IP)
		return (0);
	ip = (const struct ip *)(const void *)(frame + sizeof(*eh));
	if (ip->ip_v != 4 || ip->ip_p != IPPROTO_UDP)
		return (0);
	udp = (const struct udphdr *)(const void *)
	    (frame + sizeof(*eh) + (ip->ip_hl * 4));
	if (ntohs(udp->uh_dport) != DHCP_CLIENT_PORT)
		return (0);
	dh = (const struct dhcp *)(const void *)
	    ((const uint8_t *)udp + sizeof(*udp));
	if (dh->dp_op != BOOTREPLY)
		return (0);
	if (ntohl(dh->dp_xid) != want_xid)
		return (0);

	opts = (const uint8_t *)dh + sizeof(*dh);
	opts_len = frame + frame_len - opts;
	if (opts_len < 4 || memcmp(opts, cookie, 4) != 0)
		return (0);
	if (dhcp_msgtype(opts, opts_len) != DHCP_MTYPE_OFFER)
		return (0);

	yiaddr_out->s_addr = dh->dp_yiaddr.s_addr;
	server_out->s_addr = 0;
	v = dhcp_option(opts, opts_len, DHCP_OPT_SERVER_ID, &l);
	if (v != NULL && l == 4)
		(void)memcpy(&server_out->s_addr, v, 4);
	return (1);
}

int
dhcp_discover_run(const char *ifname)
{
	uint8_t mac[6];
	uint8_t frame[1024];
	uint8_t rxbuf[4096];
	uint32_t xid;
	int bpf_fd;
	size_t frame_len;
	struct timespec ts;
	struct in_addr yiaddr, server;
	time_t deadline;
	int got = 0;

	if (get_interface_mac(ifname, mac) != 0) {
		xlog("IPCFG-DISCOVER-FAIL: get_interface_mac(%s): %s",
		    ifname, strerror(errno));
		return (-1);
	}
	xlog("iface %s mac %02x:%02x:%02x:%02x:%02x:%02x",
	    ifname, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	if (bring_interface_up(ifname) != 0) {
		xlog("IPCFG-DISCOVER-FAIL: bring_interface_up(%s): %s",
		    ifname, strerror(errno));
		return (-1);
	}

	bpf_fd = bpf_open(ifname);
	if (bpf_fd < 0) {
		xlog("IPCFG-DISCOVER-FAIL: bpf_open(%s): %s",
		    ifname, strerror(errno));
		return (-1);
	}

	(void)clock_gettime(CLOCK_REALTIME, &ts);
	xid = (uint32_t)ts.tv_nsec ^ (uint32_t)ts.tv_sec ^
	    (uint32_t)(mac[5] | (mac[4] << 8));

	frame_len = build_discover(frame, mac, xid);
	if (write(bpf_fd, frame, frame_len) != (ssize_t)frame_len) {
		xlog("IPCFG-DISCOVER-FAIL: write(bpf, %zu): %s",
		    frame_len, strerror(errno));
		(void)close(bpf_fd);
		return (-1);
	}
	xlog("sent DHCPDISCOVER (xid=0x%08x, len=%zu)", xid, frame_len);

	/* Read BPF responses until we get a matching DHCPOFFER or
	 * 10 seconds elapse — the BPF descriptor has a 5s read
	 * timeout so this is two read attempts. */
	deadline = time(NULL) + 10;
	while (!got && time(NULL) < deadline) {
		ssize_t n = read(bpf_fd, rxbuf, sizeof(rxbuf));
		uint8_t *p, *end;

		if (n <= 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			break;	/* timeout or error */
		}
		p = rxbuf;
		end = rxbuf + n;
		/* BPF return buffer can carry multiple packets, each
		 * preceded by a struct bpf_hdr and aligned. */
		while (p + sizeof(struct bpf_hdr) <= end) {
			struct bpf_hdr bh;
			uint8_t *pkt;
			size_t caplen;

			(void)memcpy(&bh, p, sizeof(bh));
			pkt = p + bh.bh_hdrlen;
			caplen = bh.bh_caplen;
			if (pkt + caplen > end)
				break;
			if (parse_offer(pkt, caplen, xid, &yiaddr,
			    &server)) {
				got = 1;
				break;
			}
			p += BPF_WORDALIGN(bh.bh_hdrlen + caplen);
		}
	}
	(void)close(bpf_fd);

	if (got) {
		char y[INET_ADDRSTRLEN], s[INET_ADDRSTRLEN];

		(void)inet_ntop(AF_INET, &yiaddr, y, sizeof(y));
		(void)inet_ntop(AF_INET, &server, s, sizeof(s));
		xlog("IPCFG-DISCOVER-OK: iface=%s xid=0x%08x yiaddr=%s "
		    "server=%s", ifname, xid, y, s);
		return (0);
	}
	xlog("IPCFG-DISCOVER-FAIL: no DHCPOFFER received within 10s "
	    "(iface=%s xid=0x%08x)", ifname, xid);
	return (-1);
}
