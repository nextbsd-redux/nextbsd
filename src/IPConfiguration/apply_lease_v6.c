/*
 * apply_lease_v6.c — install a SLAAC address + default route.
 *
 * Three sub-steps:
 *   1. Compute the IID. RFC 4862 §5.5.3 modified-EUI-64: prefix(64)
 *      || mac[0..3] || ff || fe || mac[3..6], with the universal/local
 *      bit (mac[0] bit 1) inverted.
 *   2. SIOCAIFADDR_IN6 with the prefix + lifetimes from the RA's PIO.
 *      The kernel performs DAD in the background; iter 7a does not
 *      wait for it.
 *   3. RTM_ADD default IPv6 route via the RA's source link-local. The
 *      gateway sockaddr_in6 carries sin6_scope_id = ifindex so the
 *      kernel knows which interface the LL belongs to.
 *
 * Deferred for later iters: lifetime renewal, RDNSS, multiple
 * prefixes, RFC 7217 stable privacy addresses (iter 7a uses
 * deterministic EUI-64; the privacy mode is configurable in Apple's
 * IPConfiguration but tied to per-interface policy that doesn't
 * exist yet here).
 */
#include "apply_lease_v6.h"

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet6/in6_var.h>
#include <netinet6/nd6.h>

#include <arpa/inet.h>

#include <errno.h>
#include <ifaddrs.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "ipconfigd[apply6] ");
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

static void
make_slaac_addr(const struct in6_addr *prefix, uint8_t prefix_len,
    const uint8_t mac[6], struct in6_addr *out)
{
	uint8_t iid[8];

	(void)prefix_len;	/* iter 7a requires /64; assert in caller */

	iid[0] = mac[0] ^ 0x02;	/* flip universal/local */
	iid[1] = mac[1];
	iid[2] = mac[2];
	iid[3] = 0xff;
	iid[4] = 0xfe;
	iid[5] = mac[3];
	iid[6] = mac[4];
	iid[7] = mac[5];

	*out = *prefix;
	(void)memcpy(&out->s6_addr[8], iid, 8);
}

static void
make_prefix_mask(uint8_t prefix_len, struct in6_addr *mask)
{
	int i;

	(void)memset(mask, 0, sizeof(*mask));
	for (i = 0; i < prefix_len && i < 128; i++)
		mask->s6_addr[i / 8] |= (uint8_t)(0x80 >> (i % 8));
}

/*
 * Compute the EUI-64 modified-IID for `mac` into `iid` (RFC 4862
 * §A "Creating Modified EUI-64 Format Interface Identifiers").
 */
static void
mac_to_eui64(const uint8_t mac[6], uint8_t iid[8])
{
	iid[0] = mac[0] ^ 0x02;
	iid[1] = mac[1];
	iid[2] = mac[2];
	iid[3] = 0xff;
	iid[4] = 0xfe;
	iid[5] = mac[3];
	iid[6] = mac[4];
	iid[7] = mac[5];
}

int
bring_v6_up(const char *ifname)
{
	struct in6_ndireq ndi;
	struct in6_aliasreq req;
	uint8_t mac[6], iid[8];
	int sock, rc, had_disabled;

	if (get_iface_mac(ifname, mac) != 0) {
		xlog("get_iface_mac(%s) failed", ifname);
		return (-1);
	}

	sock = socket(AF_INET6, SOCK_DGRAM, 0);
	if (sock < 0) {
		xlog("socket(AF_INET6) failed: %s", strerror(errno));
		return (-1);
	}

	/*
	 * Read current nd6 flags. ND6_IFF_IFDISABLED is the bit FreeBSD
	 * sets when net.inet6.ip6.auto_linklocal is 0 at attach. Clear
	 * it (also enable ACCEPT_RTADV so kernel-side ND processing
	 * isn't surprised when our RA arrives) and write back.
	 */
	(void)memset(&ndi, 0, sizeof(ndi));
	(void)strlcpy(ndi.ifname, ifname, sizeof(ndi.ifname));
	had_disabled = 0;
	if (ioctl(sock, SIOCGIFINFO_IN6, &ndi) == 0) {
		had_disabled = (ndi.ndi.flags & ND6_IFF_IFDISABLED) != 0;
		ndi.ndi.flags &= ~ND6_IFF_IFDISABLED;
		ndi.ndi.flags |= ND6_IFF_ACCEPT_RTADV;
		if (ioctl(sock, SIOCSIFINFO_IN6, &ndi) != 0)
			xlog("SIOCSIFINFO_IN6(%s) failed: %s (continuing)",
			    ifname, strerror(errno));
		else if (had_disabled)
			xlog("cleared ND6_IFF_IFDISABLED on %s", ifname);
	} else {
		xlog("SIOCGIFINFO_IN6(%s) failed: %s (continuing)",
		    ifname, strerror(errno));
	}

	/*
	 * Install fe80::<EUI-64>/64. If the kernel cleared IFDISABLED
	 * and auto-added one in the moment between our two ioctls, this
	 * returns EEXIST — benign.
	 */
	(void)memset(&req, 0, sizeof(req));
	(void)strlcpy(req.ifra_name, ifname, sizeof(req.ifra_name));
	req.ifra_addr.sin6_family = AF_INET6;
	req.ifra_addr.sin6_len = sizeof(struct sockaddr_in6);
	req.ifra_addr.sin6_addr.s6_addr[0] = 0xfe;
	req.ifra_addr.sin6_addr.s6_addr[1] = 0x80;
	mac_to_eui64(mac, iid);
	(void)memcpy(&req.ifra_addr.sin6_addr.s6_addr[8], iid, 8);

	req.ifra_prefixmask.sin6_family = AF_INET6;
	req.ifra_prefixmask.sin6_len = sizeof(struct sockaddr_in6);
	make_prefix_mask(64, &req.ifra_prefixmask.sin6_addr);

	req.ifra_flags = IN6_IFF_AUTOCONF;
	req.ifra_lifetime.ia6t_vltime = ND6_INFINITE_LIFETIME;
	req.ifra_lifetime.ia6t_pltime = ND6_INFINITE_LIFETIME;

	rc = ioctl(sock, SIOCAIFADDR_IN6, &req);
	if (rc != 0 && errno != EEXIST) {
		xlog("SIOCAIFADDR_IN6(%s, fe80::…) failed: %s",
		    ifname, strerror(errno));
		(void)close(sock);
		return (-1);
	}
	(void)close(sock);

	{
		char buf[INET6_ADDRSTRLEN];

		(void)inet_ntop(AF_INET6, &req.ifra_addr.sin6_addr, buf,
		    sizeof(buf));
		xlog("link-local %s installed on %s%s", buf, ifname,
		    rc != 0 ? " (already present)" : "");
	}
	return (0);
}

static int
install_v6_address(const char *ifname, const struct in6_addr *addr,
    uint8_t prefix_len, uint32_t valid_lt, uint32_t preferred_lt)
{
	struct in6_aliasreq req;
	int sock, rc;

	(void)memset(&req, 0, sizeof(req));
	(void)strlcpy(req.ifra_name, ifname, sizeof(req.ifra_name));

	req.ifra_addr.sin6_family = AF_INET6;
	req.ifra_addr.sin6_len = sizeof(struct sockaddr_in6);
	req.ifra_addr.sin6_addr = *addr;

	req.ifra_prefixmask.sin6_family = AF_INET6;
	req.ifra_prefixmask.sin6_len = sizeof(struct sockaddr_in6);
	make_prefix_mask(prefix_len, &req.ifra_prefixmask.sin6_addr);

	/*
	 * Apple's IPConfiguration sets ifra_flags=IN6_IFF_AUTOCONF here.
	 * FreeBSD's in6_var.h exposes the same flag; treating an
	 * RA-derived address as autoconf lets the kernel apply RFC 4862
	 * lifetime semantics (auto-remove on valid expiry) if we ever
	 * wire that. For iter 7a the daemon doesn't track expiry, but
	 * setting the flag still matches the address's actual provenance.
	 */
	req.ifra_flags = IN6_IFF_AUTOCONF;

	req.ifra_lifetime.ia6t_vltime = valid_lt;
	req.ifra_lifetime.ia6t_pltime = preferred_lt;

	sock = socket(AF_INET6, SOCK_DGRAM, 0);
	if (sock < 0) {
		xlog("socket(AF_INET6) failed: %s", strerror(errno));
		return (-1);
	}
	rc = ioctl(sock, SIOCAIFADDR_IN6, &req);
	if (rc != 0)
		xlog("SIOCAIFADDR_IN6(%s) failed: %s", ifname, strerror(errno));
	(void)close(sock);
	return (rc);
}

/*
 * SA_SIZE-equivalent rounding for sockaddr_in6 in a PF_ROUTE message:
 * the kernel reads sockaddrs at sizeof(long)-aligned offsets, so each
 * sin6 occupies 32 bytes on the wire (28-byte struct + 4 bytes
 * padding). IPv4's sockaddr_in is 16 bytes — already aligned — which
 * is why apply_lease.c's IPv4 RTM_ADD works without padding.
 */
#define RT_SIN6_WIRE	32
#if RT_SIN6_WIRE < sizeof(struct sockaddr_in6)
#error "RT_SIN6_WIRE smaller than struct sockaddr_in6"
#endif

static int
install_v6_default_route(const struct in6_addr *gateway, unsigned ifindex)
{
	struct {
		struct rt_msghdr	hdr;
		uint8_t			dst[RT_SIN6_WIRE];
		uint8_t			gw[RT_SIN6_WIRE];
		uint8_t			mask[RT_SIN6_WIRE];
	} msg;
	struct sockaddr_in6 sa;
	int sock;
	ssize_t n;

	(void)memset(&msg, 0, sizeof(msg));
	msg.hdr.rtm_msglen = sizeof(msg);
	msg.hdr.rtm_version = RTM_VERSION;
	msg.hdr.rtm_type = RTM_ADD;
	msg.hdr.rtm_addrs = RTA_DST | RTA_GATEWAY | RTA_NETMASK;
	msg.hdr.rtm_flags = RTF_UP | RTF_GATEWAY | RTF_STATIC;
	msg.hdr.rtm_pid = (int32_t)getpid();
	msg.hdr.rtm_seq = 1;

	/* dst = ::/0 */
	(void)memset(&sa, 0, sizeof(sa));
	sa.sin6_family = AF_INET6;
	sa.sin6_len = sizeof(sa);
	(void)memcpy(msg.dst, &sa, sizeof(sa));

	/*
	 * gw — copy the LL gateway, then embed the ifindex into bytes
	 * 2-3 of the address per FreeBSD's "embedded scope" convention
	 * for PF_ROUTE sockaddrs (sa6_embedscope). sin6_scope_id stays
	 * 0 — the kernel's rt_xaddrs path picks up scope from the
	 * embedded form. The same encoding is visible in dmesg lines
	 * like `fe80:1::5054:ff:fe12:3456(em0)` — the `:1` IS ifindex.
	 */
	(void)memset(&sa, 0, sizeof(sa));
	sa.sin6_family = AF_INET6;
	sa.sin6_len = sizeof(sa);
	sa.sin6_addr = *gateway;
	if (IN6_IS_ADDR_LINKLOCAL(&sa.sin6_addr)) {
		sa.sin6_addr.s6_addr[2] = (uint8_t)((ifindex >> 8) & 0xff);
		sa.sin6_addr.s6_addr[3] = (uint8_t)(ifindex & 0xff);
	}
	(void)memcpy(msg.gw, &sa, sizeof(sa));

	/* mask = ::/0 */
	(void)memset(&sa, 0, sizeof(sa));
	sa.sin6_family = AF_INET6;
	sa.sin6_len = sizeof(sa);
	(void)memcpy(msg.mask, &sa, sizeof(sa));

	sock = socket(AF_ROUTE, SOCK_RAW, 0);
	if (sock < 0) {
		xlog("socket(AF_ROUTE) failed: %s", strerror(errno));
		return (-1);
	}
	n = write(sock, &msg, sizeof(msg));
	if (n != (ssize_t)sizeof(msg)) {
		if (errno == EEXIST) {
			(void)close(sock);
			return (0);
		}
		xlog("write(PF_ROUTE, RTM_ADD ::/0) failed: %s",
		    strerror(errno));
		(void)close(sock);
		return (-1);
	}
	(void)close(sock);
	return (0);
}

int
apply_ra_lease(const char *ifname, const struct ra_info *info,
    struct in6_addr *out_addr)
{
	uint8_t mac[6];
	struct in6_addr slaac;
	char abuf[INET6_ADDRSTRLEN];

	if (info->prefix_len != 64) {
		xlog("PIO prefix_len=%u != 64 — SLAAC requires /64 "
		    "(RFC 4862 §5.5.3); skipping",
		    (unsigned)info->prefix_len);
		return (-1);
	}
	if (get_iface_mac(ifname, mac) != 0) {
		xlog("get_iface_mac(%s) failed", ifname);
		return (-1);
	}

	make_slaac_addr(&info->prefix, info->prefix_len, mac, &slaac);
	(void)inet_ntop(AF_INET6, &slaac, abuf, sizeof(abuf));
	xlog("SLAAC address: %s/%u (EUI-64 from %s MAC)",
	    abuf, (unsigned)info->prefix_len, ifname);

	if (install_v6_address(ifname, &slaac, info->prefix_len,
	    info->valid_lifetime, info->preferred_lifetime) != 0)
		return (-1);

	*out_addr = slaac;

	/*
	 * Route install is best-effort. If it fails (e.g. EEXIST from a
	 * stale ::/0 default) we still report success — the address is
	 * up and reachable on-link.
	 */
	if (install_v6_default_route(&info->router_lladdr,
	    info->router_scope_id) != 0)
		xlog("default ::/0 route install failed (non-fatal)");

	return (0);
}
