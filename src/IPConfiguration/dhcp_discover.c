/*
 * dhcp_discover.c — DHCPv4 client front door (iter 3).
 *
 * iter 2 shipped a one-shot DISCOVER/OFFER probe. iter 3 grows it
 * into the full RFC 2131 INIT → SELECTING → REQUESTING → BOUND
 * exchange: brings the iface up, opens BPF, sends DHCPDISCOVER,
 * receives DHCPOFFER, sends DHCPREQUEST referencing the offer's
 * server-id + yiaddr, receives DHCPACK, parses lease options
 * (subnet, router, lease time, DNS) into struct dhcp_lease.
 *
 * Both transmits use the RFC 2131 retransmit ladder (4 / 8 / 16
 * seconds with ±1s jitter). The first DISCOVER on QEMU SLIRP is
 * always answered in well under 100ms; the ladder exists so a
 * lossy real network gets standard-compliant behavior.
 *
 * BPF reads are signal-aware: SIGTERM/SIGHUP between reads or
 * during a blocking BIOCSRTIMEOUT wait short-circuits the loop
 * (the daemon's global `got_term` flag is checked between every
 * read and on EINTR). Fixes iter 2's 10-second signal deafness.
 *
 * NOT vendored from Apple's IPConfiguration.bproj. The Apple
 * bootp/bootplib/dhcp_options.c source (~1700 LOC, 113 options)
 * is the long-term destination; iter 3 keeps its own small option
 * parser for the half-dozen options BOUND needs. The vendor lift
 * naturally lands when iter 4+ pulls in Apple's lease persistence
 * + the full T1/T2 renewal machinery.
 */
#include "dhcp_discover.h"
#include "arp_probe.h"
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
		if (dl->sdl_type != 6)	/* IFT_ETHER */
			continue;
		(void)strlcpy(ifname_out, p->ifa_name, ifname_sz);
		found = 1;
		break;
	}
	freeifaddrs(ifa);
	return (found ? 0 : -1);
}

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

static int
bring_interface_up(const char *ifname)
{
	struct ifreq ifr;
	int sock, rc = -1, i;
	unsigned before, after = 0;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		return (-1);
	(void)memset(&ifr, 0, sizeof(ifr));
	(void)strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	if (ioctl(sock, SIOCGIFFLAGS, &ifr) != 0) {
		(void)close(sock);
		return (-1);
	}
	before = (unsigned)(uint16_t)ifr.ifr_flags;
	if ((ifr.ifr_flags & IFF_UP) == 0) {
		ifr.ifr_flags |= IFF_UP;
		rc = ioctl(sock, SIOCSIFFLAGS, &ifr);
		if (rc != 0) {
			xlog("SIOCSIFFLAGS(%s, +IFF_UP) failed: %s",
			    ifname, strerror(errno));
			(void)close(sock);
			return (rc);
		}
	} else {
		rc = 0;
	}
	for (i = 0; i < 30; i++) {
		(void)memset(&ifr, 0, sizeof(ifr));
		(void)strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
		if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
			after = (unsigned)(uint16_t)ifr.ifr_flags;
			if ((after & IFF_UP) != 0)
				break;
		}
		(void)usleep(100000);
	}
	xlog("bring_interface_up(%s) flags=0x%x -> 0x%x (waited %d/30)",
	    ifname, before, after, i);
	(void)close(sock);
	return (rc);
}

static int
bpf_open(const char *ifname)
{
	struct ifreq ifr;
	int fd, i;
	u_int one = 1;
	struct timeval tv;

	fd = open("/dev/bpf", O_RDWR);
	if (fd < 0 && (errno == ENOENT || errno == ENXIO)) {
		for (i = 0; i < 256; i++) {
			char path[32];

			(void)snprintf(path, sizeof(path), "/dev/bpf%d", i);
			fd = open(path, O_RDWR);
			if (fd >= 0)
				break;
			if (errno != EBUSY)
				break;
		}
	}
	if (fd < 0)
		return (-1);

	(void)memset(&ifr, 0, sizeof(ifr));
	(void)strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	if (ioctl(fd, BIOCSETIF, &ifr) != 0) {
		(void)close(fd);
		return (-1);
	}
	(void)ioctl(fd, BIOCIMMEDIATE, &one);
	(void)ioctl(fd, BIOCSHDRCMPLT, &one);
	/* BPF read timeout: 1s. Each timeout returns to the caller so
	 * the wait loop can rotate through got_term + retransmit
	 * deadline checks. */
	tv.tv_sec = 1;
	tv.tv_usec = 0;
	(void)ioctl(fd, BIOCSRTIMEOUT, &tv);
	return (fd);
}

/*
 * Build a DHCPDISCOVER or DHCPREQUEST frame into `buf` (≥ 1024
 * bytes). For DISCOVER, requested_ip / server_id / ciaddr are all
 * NULL. For a SELECTING-state REQUEST, requested_ip + server_id
 * come from the OFFER we just received and ciaddr is NULL. For a
 * RENEWING/REBINDING REQUEST (iter 4), requested_ip + server_id are
 * NULL but ciaddr is set to the bound address — RFC 2131 §4.3.2
 * encoding for the post-BOUND renewal request. Returns the total
 * Ethernet frame length.
 */
static size_t
build_dhcp_msg(uint8_t *buf, const uint8_t mac[6], uint32_t xid,
    uint8_t msgtype, const struct in_addr *requested_ip,
    const struct in_addr *server_id, const struct in_addr *ciaddr)
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
	ip->ip_ttl = 64;
	ip->ip_p = IPPROTO_UDP;
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
	if (ciaddr != NULL)
		dh->dp_ciaddr = *ciaddr;
	(void)memcpy(dh->dp_chaddr, mac, 6);

	opt = (uint8_t *)dh + sizeof(*dh);
	(void)memcpy(opt, cookie, 4);
	opt += 4;

	/* 53 message type */
	*opt++ = DHCP_OPT_MESSAGE_TYPE;
	*opt++ = 1;
	*opt++ = msgtype;

	/* 61 client id = htype + mac */
	*opt++ = DHCP_OPT_CLIENT_ID;
	*opt++ = 7;
	*opt++ = HTYPE_ETHERNET;
	(void)memcpy(opt, mac, 6);
	opt += 6;

	/* REQUEST + DECLINE: option 50 (requested ip) + 54 (server id).
	 * RFC 2131 §4.3.2 requires these on the SELECTING-state REQUEST
	 * so the server can disambiguate which offer the client took.
	 * RFC 2131 §3.1.5 requires them on DHCPDECLINE so the server
	 * knows which offer the client is rejecting (so it can mark the
	 * address as in-use and not re-offer it). */
	if (msgtype == DHCP_MTYPE_REQUEST || msgtype == DHCP_MTYPE_DECLINE) {
		if (requested_ip != NULL) {
			*opt++ = DHCP_OPT_REQUESTED_IP;
			*opt++ = 4;
			(void)memcpy(opt, &requested_ip->s_addr, 4);
			opt += 4;
		}
		if (server_id != NULL) {
			*opt++ = DHCP_OPT_SERVER_ID;
			*opt++ = 4;
			(void)memcpy(opt, &server_id->s_addr, 4);
			opt += 4;
		}
	}

	/* 12 host name — advertise our hostname so the DHCP server (the
	 * home router, typically) registers it in LAN DNS. Without this the
	 * router has no name to map, so neither the bare hostname nor
	 * <hostname>.<router-domain> (e.g. .home.local) resolves. RFC 2132
	 * §3.14: the option carries the client's name; we send the short
	 * (label-only) form, which is what routers register. Sent on
	 * DISCOVER and REQUEST (incl. renew); skipped on DECLINE/RELEASE.
	 * gethostname(3) returns the synthesised name launchd PID-1 set at
	 * early init (see hostnamed / freebsd_synthesize_hostname). */
	if (msgtype == DHCP_MTYPE_DISCOVER || msgtype == DHCP_MTYPE_REQUEST) {
		char hn[256];

		if (gethostname(hn, sizeof(hn)) == 0 && hn[0] != '\0') {
			char *dot = strchr(hn, '.');
			size_t hlen;

			if (dot != NULL)
				*dot = '\0';	/* strip domain — label only */
			hlen = strlen(hn);
			if (hlen > 0 && hlen <= 255) {
				*opt++ = DHCP_OPT_HOST_NAME;
				*opt++ = (uint8_t)hlen;
				(void)memcpy(opt, hn, hlen);
				opt += hlen;
			}
		}
	}

	/* 55 parameter request list — what we want in the OFFER/ACK. */
	*opt++ = DHCP_OPT_PARAM_REQUEST;
	*opt++ = 5;
	*opt++ = DHCP_OPT_SUBNET_MASK;
	*opt++ = DHCP_OPT_ROUTER;
	*opt++ = DHCP_OPT_DNS_SERVER;
	*opt++ = DHCP_OPT_LEASE_TIME;
	*opt++ = DHCP_OPT_SERVER_ID;
	*opt++ = DHCP_OPT_END;

	/* Pad to RFC-2131 minimum 576 IP bytes. */
	frame_len = (size_t)(opt - buf);
	while (frame_len < sizeof(*eh) + DHCP_PACKET_MIN)
		buf[frame_len++] = DHCP_OPT_PAD;

	udp_len = frame_len - sizeof(*eh) - sizeof(*ip);
	ip_len = frame_len - sizeof(*eh);
	ip->ip_len = htons((u_short)ip_len);
	udp->uh_ulen = htons((u_short)udp_len);

	ip->ip_sum = 0;
	ip->ip_sum = htons(in_cksum(ip, sizeof(*ip)));

	(void)memset(pseudo, 0, sizeof(pseudo));
	(void)memcpy(pseudo, &ip->ip_src.s_addr, 4);
	(void)memcpy(pseudo + 4, &ip->ip_dst.s_addr, 4);
	pseudo[9] = IPPROTO_UDP;
	pseudo[10] = (uint8_t)(udp_len >> 8);
	pseudo[11] = (uint8_t)(udp_len & 0xff);
	{
		uint8_t tmp[2048];

		if (udp_len <= sizeof(tmp) - sizeof(pseudo)) {
			(void)memcpy(tmp, pseudo, sizeof(pseudo));
			(void)memcpy(tmp + sizeof(pseudo), udp, udp_len);
			udp->uh_sum = htons(in_cksum(tmp,
			    sizeof(pseudo) + udp_len));
			if (udp->uh_sum == 0)
				udp->uh_sum = 0xffff;
		}
	}
	return (frame_len);
}

/*
 * Find DHCP option `code` in the options blob (after the cookie).
 * Returns pointer + length, or NULL on absence.
 */
static const uint8_t *
dhcp_option(const uint8_t *opts, size_t opts_len, uint8_t code,
    uint8_t *len_out)
{
	size_t i = 4;	/* skip the magic cookie */

	while (i + 1 < opts_len) {
		uint8_t c = opts[i], l;

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
 * Parse a BPF-delivered frame as a DHCP reply matching want_xid +
 * want_msgtype. On match: fills `lease` with everything we care
 * about (addr / netmask / router / server / lease_time / DNS) and
 * returns 1. Returns 0 on a non-match (skip; caller continues
 * the read loop).
 */
static int
parse_dhcp_reply(const uint8_t *frame, size_t frame_len,
    uint32_t want_xid, uint8_t want_msgtype, struct dhcp_lease *lease)
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
	opts_len = (size_t)(frame + frame_len - opts);
	if (opts_len < 4 || memcmp(opts, cookie, 4) != 0)
		return (0);

	/* Message type */
	v = dhcp_option(opts, opts_len, DHCP_OPT_MESSAGE_TYPE, &l);
	if (v == NULL || l != 1 || *v != want_msgtype)
		return (0);

	(void)memset(lease, 0, sizeof(*lease));
	lease->addr.s_addr = dh->dp_yiaddr.s_addr;

	/* 54 server id */
	v = dhcp_option(opts, opts_len, DHCP_OPT_SERVER_ID, &l);
	if (v != NULL && l == 4)
		(void)memcpy(&lease->server.s_addr, v, 4);

	/* 1 subnet mask (default /8 if absent — RFC silly fallback) */
	v = dhcp_option(opts, opts_len, DHCP_OPT_SUBNET_MASK, &l);
	if (v != NULL && l == 4)
		(void)memcpy(&lease->netmask.s_addr, v, 4);
	else
		lease->netmask.s_addr = htonl(0xff000000);

	/* 3 router (first only) */
	v = dhcp_option(opts, opts_len, DHCP_OPT_ROUTER, &l);
	if (v != NULL && l >= 4)
		(void)memcpy(&lease->router.s_addr, v, 4);

	/* 51 lease time */
	v = dhcp_option(opts, opts_len, DHCP_OPT_LEASE_TIME, &l);
	if (v != NULL && l == 4) {
		uint32_t be;

		(void)memcpy(&be, v, 4);
		lease->lease_time = ntohl(be);
	}

	/* 6 DNS — list of IPv4 addrs, up to DHCP_LEASE_MAX_DNS */
	v = dhcp_option(opts, opts_len, DHCP_OPT_DNS_SERVER, &l);
	if (v != NULL && l >= 4 && (l % 4) == 0) {
		unsigned n = l / 4;

		if (n > DHCP_LEASE_MAX_DNS)
			n = DHCP_LEASE_MAX_DNS;
		for (unsigned i = 0; i < n; i++)
			(void)memcpy(&lease->dns[i].s_addr, v + i * 4, 4);
		lease->dns_count = n;
	}

	/* 12 host name — server-supplied client name. Per RFC 2132 §3.14
	 * this is a plain string (NOT NUL-terminated on the wire). Truncate
	 * any over-length value to DHCP_LEASE_MAX_HOSTNAME - 1 so we keep
	 * room for a terminator; sc_publish_dhcp converts to CFString.
	 * Issue #88. */
	v = dhcp_option(opts, opts_len, DHCP_OPT_HOST_NAME, &l);
	if (v != NULL && l > 0) {
		unsigned n = (l < DHCP_LEASE_MAX_HOSTNAME - 1)
		    ? (unsigned)l : (DHCP_LEASE_MAX_HOSTNAME - 1);
		(void)memcpy(lease->host_name, v, n);
		lease->host_name[n] = '\0';
		lease->host_name_len = n;
	}

	return (1);
}

/*
 * Wait up to `deadline` (CLOCK_MONOTONIC absolute) for a DHCP
 * reply matching want_xid + want_msgtype. Returns 1 on match, 0
 * on timeout, -1 on shutdown signal or fatal read error.
 *
 * The BPF descriptor has a 1s BIOCSRTIMEOUT so the wait wakes
 * every second to re-check got_term + the deadline.
 */
static int
recv_dhcp_reply(int bpf_fd, uint32_t want_xid, uint8_t want_msgtype,
    struct timespec *deadline, struct dhcp_lease *lease)
{
	uint8_t rxbuf[4096];

	while (!got_term) {
		struct timespec now;
		ssize_t n;
		uint8_t *p, *end;

		(void)clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec > deadline->tv_sec ||
		    (now.tv_sec == deadline->tv_sec &&
		     now.tv_nsec >= deadline->tv_nsec))
			return (0);

		n = read(bpf_fd, rxbuf, sizeof(rxbuf));
		if (n < 0) {
			if (errno == EINTR && got_term)
				return (-1);
			if (errno == EAGAIN || errno == EINTR)
				continue;
			xlog("read(bpf) failed: %s", strerror(errno));
			return (-1);
		}
		if (n == 0)
			continue;	/* BPF read timeout */

		p = rxbuf;
		end = rxbuf + n;
		while (p + sizeof(struct bpf_hdr) <= end) {
			struct bpf_hdr bh;
			uint8_t *pkt;
			size_t caplen;

			(void)memcpy(&bh, p, sizeof(bh));
			pkt = p + bh.bh_hdrlen;
			caplen = bh.bh_caplen;
			if (pkt + caplen > end)
				break;
			if (parse_dhcp_reply(pkt, caplen, want_xid,
			    want_msgtype, lease))
				return (1);
			p += BPF_WORDALIGN(bh.bh_hdrlen + caplen);
		}
	}
	return (-1);
}

/*
 * Send a DHCP message, handling em(4)/iflib's ~100ms post-IFF_UP
 * window where if_transmit returns ENETDOWN even after IFF_UP +
 * IFF_DRV_RUNNING are both set. iter 2 audited this in detail.
 */
static int
send_dhcp(int bpf_fd, const uint8_t *frame, size_t frame_len)
{
	int try;
	ssize_t n = -1;

	for (try = 0; try < 10 && !got_term; try++) {
		n = write(bpf_fd, frame, frame_len);
		if (n == (ssize_t)frame_len)
			return (0);
		if (errno != ENETDOWN)
			break;
		(void)usleep(100000);
	}
	xlog("write(bpf, %zu) failed after %d tries: %s",
	    frame_len, try + 1, strerror(errno));
	return (-1);
}

/* RFC 2131 retransmit ladder: 4s, 8s, 16s with ±1s jitter. */
static const int retry_seconds[] = { 4, 8, 16 };
#define DHCP_MAX_RETRIES	(int)(sizeof(retry_seconds) / sizeof(retry_seconds[0]))

static void
deadline_in(struct timespec *out, int seconds)
{
	long jitter_ns;

	(void)clock_gettime(CLOCK_MONOTONIC, out);
	/* ±1s jitter — RFC 2131 §4.1 recommendation. */
	jitter_ns = (long)random() % 2000000000L - 1000000000L;
	out->tv_sec += seconds;
	out->tv_nsec += jitter_ns;
	while (out->tv_nsec < 0) {
		out->tv_nsec += 1000000000L;
		out->tv_sec--;
	}
	while (out->tv_nsec >= 1000000000L) {
		out->tv_nsec -= 1000000000L;
		out->tv_sec++;
	}
}

int
dhcp_lease_acquire(const char *ifname, struct dhcp_lease *lease_out)
{
	uint8_t mac[6];
	uint8_t frame[1024];
	uint32_t xid;
	int bpf_fd, try;
	size_t frame_len;
	struct timespec deadline;
	struct dhcp_lease offer;

	if (get_interface_mac(ifname, mac) != 0) {
		xlog("get_interface_mac(%s) failed: %s",
		    ifname, strerror(errno));
		xlog("IPCFG-BOUND-FAIL");
		return (-1);
	}
	xlog("iface %s mac %02x:%02x:%02x:%02x:%02x:%02x",
	    ifname, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	if (bring_interface_up(ifname) != 0) {
		xlog("IPCFG-BOUND-FAIL");
		return (-1);
	}

	bpf_fd = bpf_open(ifname);
	if (bpf_fd < 0) {
		xlog("bpf_open(%s) failed: %s", ifname, strerror(errno));
		xlog("IPCFG-BOUND-FAIL");
		return (-1);
	}

	{
		struct timespec ts;

		(void)clock_gettime(CLOCK_REALTIME, &ts);
		xid = (uint32_t)ts.tv_nsec ^ (uint32_t)ts.tv_sec ^
		    (uint32_t)(mac[5] | (mac[4] << 8));
		srandom((unsigned)xid);
	}

	/* ---- SELECTING: send DISCOVER until an OFFER arrives ---- */
	(void)memset(&offer, 0, sizeof(offer));
	for (try = 0; try < DHCP_MAX_RETRIES; try++) {
		int r;

		if (got_term)
			goto shutdown;
		frame_len = build_dhcp_msg(frame, mac, xid,
		    DHCP_MTYPE_DISCOVER, NULL, NULL, NULL);
		if (send_dhcp(bpf_fd, frame, frame_len) != 0) {
			xlog("IPCFG-BOUND-FAIL");
			(void)close(bpf_fd);
			return (-1);
		}
		xlog("sent DHCPDISCOVER (xid=0x%08x, try=%d/%d)", xid,
		    try + 1, DHCP_MAX_RETRIES);

		deadline_in(&deadline, retry_seconds[try]);
		r = recv_dhcp_reply(bpf_fd, xid, DHCP_MTYPE_OFFER,
		    &deadline, &offer);
		if (r == 1)
			break;
		if (r < 0)
			goto shutdown;
	}
	if (try == DHCP_MAX_RETRIES) {
		xlog("no DHCPOFFER after %d retries (iface=%s xid=0x%08x)",
		    DHCP_MAX_RETRIES, ifname, xid);
		xlog("IPCFG-BOUND-FAIL");
		(void)close(bpf_fd);
		return (-1);
	}
	{
		char y[INET_ADDRSTRLEN], s[INET_ADDRSTRLEN];

		(void)inet_ntop(AF_INET, &offer.addr, y, sizeof(y));
		(void)inet_ntop(AF_INET, &offer.server, s, sizeof(s));
		xlog("OFFER yiaddr=%s server=%s", y, s);
	}

	/* ---- RFC 5227 ARP probe on the offered address ----
	 *
	 * Defends against the DHCP server handing out an address that
	 * another host on the segment is already using (a misconfigured
	 * static client, a leaked lease, etc.). On conflict we DECLINE
	 * the offer and bail — iter 6 doesn't loop back to INIT (that
	 * needs a per-attempt xid + a §3.1.5-mandated 10s wait between
	 * decline and a new DISCOVER, which is iter 7 work). The daemon
	 * stays alive on its Mach service; CI's SLIRP gateway can't
	 * actually replicate a conflict, so the conflict branch is
	 * exercised only by the unit/logic path. */
	{
		int pr = arp_probe(ifname, offer.addr);

		if (pr == 1) {
			frame_len = build_dhcp_msg(frame, mac, xid,
			    DHCP_MTYPE_DECLINE, &offer.addr, &offer.server,
			    NULL);
			if (send_dhcp(bpf_fd, frame, frame_len) != 0)
				xlog("DHCPDECLINE send failed");
			else
				xlog("sent DHCPDECLINE (xid=0x%08x)", xid);
			xlog("IPCFG-BOUND-FAIL");
			(void)close(bpf_fd);
			return (-1);
		}
		if (pr == 0)
			xlog("IPCFG-ARP-OK");
		/* pr < 0: arp_probe failed (BPF open / send error). Treat
		 * as no-conflict so a transient probe issue doesn't
		 * deadlock the lease; the address is still verified by
		 * DHCPACK below. No marker emitted on this path — the
		 * boot test fails fast if it never sees ARP-OK. */
	}

	/* ---- REQUESTING: send REQUEST until an ACK arrives ---- */
	for (try = 0; try < DHCP_MAX_RETRIES; try++) {
		int r;

		if (got_term)
			goto shutdown;
		frame_len = build_dhcp_msg(frame, mac, xid,
		    DHCP_MTYPE_REQUEST, &offer.addr, &offer.server, NULL);
		if (send_dhcp(bpf_fd, frame, frame_len) != 0) {
			xlog("IPCFG-BOUND-FAIL");
			(void)close(bpf_fd);
			return (-1);
		}
		xlog("sent DHCPREQUEST (xid=0x%08x, try=%d/%d)", xid,
		    try + 1, DHCP_MAX_RETRIES);

		deadline_in(&deadline, retry_seconds[try]);
		r = recv_dhcp_reply(bpf_fd, xid, DHCP_MTYPE_ACK,
		    &deadline, lease_out);
		if (r == 1) {
			(void)close(bpf_fd);
			return (0);
		}
		if (r < 0)
			goto shutdown;
	}
	xlog("no DHCPACK after %d retries (iface=%s xid=0x%08x)",
	    DHCP_MAX_RETRIES, ifname, xid);
	xlog("IPCFG-BOUND-FAIL");
	(void)close(bpf_fd);
	return (-1);

shutdown:
	xlog("DHCP exchange interrupted by signal (got_term=%d)",
	    (int)got_term);
	xlog("IPCFG-BOUND-FAIL");
	(void)close(bpf_fd);
	return (-1);
}

int
dhcp_renew(const char *ifname, const struct dhcp_lease *existing,
    struct dhcp_lease *new_lease)
{
	uint8_t mac[6];
	uint8_t frame[1024];
	uint32_t xid;
	int bpf_fd, try;
	size_t frame_len;
	struct timespec deadline;

	if (get_interface_mac(ifname, mac) != 0)
		return (-1);

	/* Interface is already up (we have a lease on it) — no
	 * bring_interface_up call needed. Open a fresh BPF descriptor
	 * just for this exchange so we don't have to plumb the fd
	 * across the lease loop. */
	bpf_fd = bpf_open(ifname);
	if (bpf_fd < 0) {
		xlog("renew: bpf_open(%s) failed: %s", ifname,
		    strerror(errno));
		return (-1);
	}

	{
		struct timespec ts;

		(void)clock_gettime(CLOCK_REALTIME, &ts);
		xid = (uint32_t)ts.tv_nsec ^ (uint32_t)ts.tv_sec ^
		    (uint32_t)(mac[5] | (mac[4] << 8));
	}

	for (try = 0; try < DHCP_MAX_RETRIES; try++) {
		int r;

		if (got_term)
			break;
		frame_len = build_dhcp_msg(frame, mac, xid,
		    DHCP_MTYPE_REQUEST, NULL, NULL, &existing->addr);
		if (send_dhcp(bpf_fd, frame, frame_len) != 0) {
			(void)close(bpf_fd);
			return (-1);
		}
		xlog("renew: sent DHCPREQUEST (xid=0x%08x, try=%d/%d)",
		    xid, try + 1, DHCP_MAX_RETRIES);

		deadline_in(&deadline, retry_seconds[try]);
		r = recv_dhcp_reply(bpf_fd, xid, DHCP_MTYPE_ACK,
		    &deadline, new_lease);
		if (r == 1) {
			(void)close(bpf_fd);
			return (0);
		}
		if (r < 0)
			break;
	}
	xlog("renew: no DHCPACK after %d retries (iface=%s xid=0x%08x)",
	    DHCP_MAX_RETRIES, ifname, xid);
	(void)close(bpf_fd);
	return (-1);
}
