/*
 * ra_listen.c — IPv6 Router Solicitation + Router Advertisement
 * receive on a single interface.
 *
 * QEMU SLIRP (the CI target) sends unsolicited RAs only every 600s
 * but responds to a solicitation within ~ms; iter 7a therefore
 * actively solicits rather than waiting for the next unsolicited.
 *
 * Send path: ICMPv6 raw socket bound to `ifname`'s scope, sendto
 * ff02::2 (all-routers multicast). Packet body = ND_ROUTER_SOLICIT
 * (RFC 4861 §4.1): 8-byte ICMP6 header with reserved=0, followed by
 * one source-link-layer-address option (type=1, len=1, our MAC).
 * The kernel computes the ICMPv6 checksum automatically for
 * IPPROTO_ICMPV6 raw sockets.
 *
 * Receive path: ICMP6_FILTER allows only ND_ROUTER_ADVERT, then
 * recvmsg with IPV6_PKTINFO + IPV6_HOPLIMIT cmsgs. RFC 4861 §6.1.2
 * requires hop-limit == 255 on the received frame (anti-spoofing).
 * Walk options after the 16-byte RA header looking for the first
 * Prefix Information Option (type=3, len=4) with on-link + autonomous
 * flags set; copy out its prefix + lifetimes + the RA's source
 * link-local address (the router for default-route purposes).
 *
 * One-shot for iter 7a — no lease loop, no expiry tracking. The
 * post-iter productionisation backlog tracks the recurring listener.
 */
#include "ra_listen.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <netinet/icmp6.h>

#include <arpa/inet.h>

#include <errno.h>
#include <ifaddrs.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * Daemon shutdown flag — let a long RA wait short-circuit on SIGTERM
 * the same way dhcp_discover / arp_probe do.
 */
extern volatile sig_atomic_t got_term;

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "ipconfigd[ra] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

static int
get_iface_mac(const char *ifname, uint8_t mac[6])
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
		dl = (const struct sockaddr_dl *)(const void *)p->ifa_addr;
		if (dl->sdl_alen != 6)
			break;
		(void)memcpy(mac, LLADDR(dl), 6);
		ok = 0;
		break;
	}
	freeifaddrs(ifa);
	return (ok);
}

/*
 * Open a raw ICMPv6 socket configured for ND host operation:
 *   - ICMP6_FILTER: pass only ND_ROUTER_ADVERT (we don't need to see
 *     our own RS coming back).
 *   - IPV6_RECVPKTINFO + IPV6_RECVHOPLIMIT: surface the ifindex and
 *     hop limit on each recvmsg so we can enforce RFC 4861 §6.1.2
 *     (hop-limit == 255 anti-spoof).
 *   - IPV6_MULTICAST_HOPS + IPV6_UNICAST_HOPS = 255: required by RFC
 *     4861 for both outbound RS and any unicast we emit.
 *   - SO_RCVTIMEO = 1s: lets the recvmsg poll loop rotate through
 *     got_term and the listen-window deadline.
 */
static int
open_icmp6(const char *ifname)
{
	struct icmp6_filter filt;
	int sock, on = 1, hops = 255;
	struct timeval tv;

	(void)ifname;	/* sendto carries the scope; nothing to bind here */

	sock = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
	if (sock < 0) {
		xlog("socket(AF_INET6, ICMPV6) failed: %s", strerror(errno));
		return (-1);
	}

	ICMP6_FILTER_SETBLOCKALL(&filt);
	ICMP6_FILTER_SETPASS(ND_ROUTER_ADVERT, &filt);
	if (setsockopt(sock, IPPROTO_ICMPV6, ICMP6_FILTER, &filt,
	    sizeof(filt)) != 0) {
		xlog("ICMP6_FILTER: %s", strerror(errno));
		(void)close(sock);
		return (-1);
	}
	(void)setsockopt(sock, IPPROTO_IPV6, IPV6_RECVPKTINFO, &on, sizeof(on));
	(void)setsockopt(sock, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &on, sizeof(on));
	(void)setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops,
	    sizeof(hops));
	(void)setsockopt(sock, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &hops,
	    sizeof(hops));

	tv.tv_sec = 1;
	tv.tv_usec = 0;
	(void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	return (sock);
}

/*
 * Send an ICMPv6 Router Solicitation to ff02::2 via `ifname`. The
 * packet is 16 bytes: 8-byte RS header + 8-byte source-LL-addr
 * option (type=1 len=1 + 6-byte MAC). Returns 0 on success.
 */
static int
send_rs(int sock, const char *ifname, const uint8_t mac[6])
{
	struct sockaddr_in6 dst;
	uint8_t pkt[16];
	struct nd_router_solicit *rs;
	uint8_t *opt;
	unsigned ifindex;
	ssize_t n;

	ifindex = if_nametoindex(ifname);
	if (ifindex == 0) {
		xlog("if_nametoindex(%s) failed: %s", ifname, strerror(errno));
		return (-1);
	}

	(void)memset(&dst, 0, sizeof(dst));
	dst.sin6_family = AF_INET6;
	dst.sin6_len = sizeof(dst);
	if (inet_pton(AF_INET6, "ff02::2", &dst.sin6_addr) != 1) {
		xlog("inet_pton(ff02::2) failed");
		return (-1);
	}
	dst.sin6_scope_id = ifindex;

	(void)memset(pkt, 0, sizeof(pkt));
	rs = (struct nd_router_solicit *)(void *)pkt;
	rs->nd_rs_type = ND_ROUTER_SOLICIT;
	rs->nd_rs_code = 0;
	rs->nd_rs_cksum = 0;	/* kernel fills */
	rs->nd_rs_reserved = 0;

	opt = pkt + 8;
	opt[0] = ND_OPT_SOURCE_LINKADDR;
	opt[1] = 1;	/* 8-byte units */
	(void)memcpy(opt + 2, mac, 6);

	n = sendto(sock, pkt, sizeof(pkt), 0,
	    (struct sockaddr *)&dst, sizeof(dst));
	if (n != (ssize_t)sizeof(pkt)) {
		xlog("sendto(ff02::2 RS) failed: %s", strerror(errno));
		return (-1);
	}
	return (0);
}

/*
 * Walk RA options starting at `opts` for `len` bytes. On the first
 * Prefix Information Option (type=3) with both on-link (L) and
 * autonomous (A) flags set, copy out prefix + lifetimes and return 1.
 * Returns 0 if no eligible PIO was found, -1 if the option chain is
 * malformed (length=0 in any option, RFC 4861 §4.6).
 */
static int
parse_options(const uint8_t *opts, size_t len, struct ra_info *out)
{
	size_t off = 0;

	while (off + 2 <= len) {
		uint8_t type = opts[off];
		uint8_t olen = opts[off + 1];
		size_t obytes;

		if (olen == 0)
			return (-1);
		obytes = (size_t)olen * 8;
		if (off + obytes > len)
			return (-1);

		if (type == ND_OPT_PREFIX_INFORMATION && obytes >= 32) {
			const struct nd_opt_prefix_info *pi;

			pi = (const struct nd_opt_prefix_info *)
			    (const void *)(opts + off);
			if ((pi->nd_opt_pi_flags_reserved &
			    (ND_OPT_PI_FLAG_ONLINK |
			     ND_OPT_PI_FLAG_AUTO)) ==
			    (ND_OPT_PI_FLAG_ONLINK |
			     ND_OPT_PI_FLAG_AUTO)) {
				out->prefix = pi->nd_opt_pi_prefix;
				out->prefix_len = pi->nd_opt_pi_prefix_len;
				out->valid_lifetime =
				    ntohl(pi->nd_opt_pi_valid_time);
				out->preferred_lifetime =
				    ntohl(pi->nd_opt_pi_preferred_time);
				return (1);
			}
		}
		off += obytes;
	}
	return (0);
}

/*
 * Single recvmsg, validate hop-limit + ifindex, parse RA. Returns
 *   1  — RA received and PIO extracted (out populated, including
 *        router_lladdr from the source address).
 *   0  — recv timed out (caller decides whether to keep waiting).
 *  -1  — fatal (shutdown or unrecoverable error).
 *
 * Note: an RA with no eligible PIO is treated as "no result" (returns
 * 0) — the caller's deadline loop will keep listening for another.
 */
static int
recv_one(int sock, unsigned want_ifindex, struct ra_info *out)
{
	uint8_t buf[1500];
	struct sockaddr_in6 from;
	struct iovec iov;
	struct msghdr mh;
	uint8_t cmsgbuf[256];
	struct cmsghdr *cm;
	ssize_t n;
	int hoplimit = -1;
	unsigned got_ifindex = 0;
	struct nd_router_advert *ra;

	iov.iov_base = buf;
	iov.iov_len = sizeof(buf);

	(void)memset(&mh, 0, sizeof(mh));
	mh.msg_name = &from;
	mh.msg_namelen = sizeof(from);
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	mh.msg_control = cmsgbuf;
	mh.msg_controllen = sizeof(cmsgbuf);

	n = recvmsg(sock, &mh, 0);
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return (0);
		if (errno == EINTR && got_term)
			return (-1);
		if (errno == EINTR)
			return (0);
		xlog("recvmsg(icmp6) failed: %s", strerror(errno));
		return (-1);
	}

	for (cm = CMSG_FIRSTHDR(&mh); cm != NULL;
	    cm = CMSG_NXTHDR(&mh, cm)) {
		if (cm->cmsg_level != IPPROTO_IPV6)
			continue;
		if (cm->cmsg_type == IPV6_PKTINFO) {
			struct in6_pktinfo pi;

			(void)memcpy(&pi, CMSG_DATA(cm), sizeof(pi));
			got_ifindex = (unsigned)pi.ipi6_ifindex;
		} else if (cm->cmsg_type == IPV6_HOPLIMIT) {
			int hl;

			(void)memcpy(&hl, CMSG_DATA(cm), sizeof(hl));
			hoplimit = hl;
		}
	}

	/*
	 * RFC 4861 §6.1.2: a host MUST silently discard an RA whose IP
	 * hop limit is not 255, or whose source is not a link-local
	 * address. Both protect against off-link spoofing.
	 */
	if (hoplimit != 255) {
		xlog("RA rejected: hop-limit=%d (want 255)", hoplimit);
		return (0);
	}
	if (!IN6_IS_ADDR_LINKLOCAL(&from.sin6_addr)) {
		xlog("RA rejected: source not link-local");
		return (0);
	}
	if (got_ifindex != 0 && got_ifindex != want_ifindex) {
		xlog("RA rejected: arrived on ifindex=%u (want %u)",
		    got_ifindex, want_ifindex);
		return (0);
	}
	if (n < (ssize_t)sizeof(struct nd_router_advert)) {
		xlog("RA rejected: short (%zd bytes)", n);
		return (0);
	}

	ra = (struct nd_router_advert *)(void *)buf;
	if (ra->nd_ra_type != ND_ROUTER_ADVERT) {
		/* The ICMP6_FILTER should make this unreachable, but
		 * defence-in-depth. */
		return (0);
	}

	(void)memset(out, 0, sizeof(*out));
	out->router_lladdr = from.sin6_addr;
	out->router_scope_id = want_ifindex;

	{
		const uint8_t *opts = buf + sizeof(struct nd_router_advert);
		size_t olen = (size_t)n - sizeof(struct nd_router_advert);
		int r = parse_options(opts, olen, out);

		if (r == 1)
			return (1);
		if (r < 0)
			xlog("RA option chain malformed; ignoring");
		else
			xlog("RA with no eligible PIO (on-link+auto); waiting");
		return (0);
	}
}

int
ra_acquire(const char *ifname, unsigned timeout_ms, struct ra_info *out)
{
	uint8_t mac[6];
	int sock, r;
	unsigned ifindex;
	struct timespec deadline, now;

	if (get_iface_mac(ifname, mac) != 0) {
		xlog("get_iface_mac(%s) failed", ifname);
		return (-1);
	}
	ifindex = if_nametoindex(ifname);
	if (ifindex == 0) {
		xlog("if_nametoindex(%s) failed: %s", ifname, strerror(errno));
		return (-1);
	}

	sock = open_icmp6(ifname);
	if (sock < 0)
		return (-1);

	if (send_rs(sock, ifname, mac) != 0) {
		(void)close(sock);
		return (-1);
	}
	xlog("sent ND_ROUTER_SOLICIT on %s (ifindex=%u) → ff02::2",
	    ifname, ifindex);

	(void)clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += timeout_ms / 1000;
	deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_nsec -= 1000000000L;
		deadline.tv_sec++;
	}

	while (!got_term) {
		(void)clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec > deadline.tv_sec ||
		    (now.tv_sec == deadline.tv_sec &&
		     now.tv_nsec >= deadline.tv_nsec)) {
			(void)close(sock);
			return (1);	/* timed out, no RA */
		}

		r = recv_one(sock, ifindex, out);
		if (r == 1) {
			char pbuf[INET6_ADDRSTRLEN];
			char rbuf[INET6_ADDRSTRLEN];

			(void)inet_ntop(AF_INET6, &out->prefix, pbuf,
			    sizeof(pbuf));
			(void)inet_ntop(AF_INET6, &out->router_lladdr, rbuf,
			    sizeof(rbuf));
			xlog("RA: prefix=%s/%u router=%s%%%s "
			    "valid=%us preferred=%us",
			    pbuf, (unsigned)out->prefix_len, rbuf, ifname,
			    (unsigned)out->valid_lifetime,
			    (unsigned)out->preferred_lifetime);
			(void)close(sock);
			return (0);
		}
		if (r < 0) {
			(void)close(sock);
			return (-1);
		}
		/* r == 0: keep listening */
	}
	(void)close(sock);
	return (-1);
}
