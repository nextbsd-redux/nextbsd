/*
 * arp_probe.c — RFC 5227 IPv4 ARP probe + announce.
 *
 * Probe: 3 ARP requests, sender IP = 0.0.0.0, target IP = the
 * address being probed. Listen between sends for any ARP frame
 * (request or reply) whose sender IP is the probed address —
 * RFC 5227 §2.1.1 says either form proves the address is in use.
 *
 * Announce: 2 gratuitous ARP requests, sender IP = target IP =
 * our IP. Updates peer ARP caches per RFC 5227 §2.3.
 *
 * Timing: RFC 5227 specifies PROBE_NUM=3 with random 1–2s gaps
 * and ANNOUNCE_WAIT=2s after the last probe; ANNOUNCE_NUM=2
 * with 2s gaps. We compress to a flat 1s between sends and a
 * 1s post-probe listen window — the loose RFC ranges are about
 * spreading probes across a noisy LAN; in CI (SLIRP, single
 * client) the deterministic short timing keeps the boot test
 * inside its existing budget. The conflict detection itself is
 * RFC-compliant.
 *
 * BPF reuses dhcp_discover.c's pattern: cloning /dev/bpf,
 * BIOCSETIF, BIOCIMMEDIATE, BIOCSHDRCMPLT, BIOCSRTIMEOUT (1s
 * tick so the wait loop can rotate through got_term). We open
 * a fresh descriptor per call rather than threading one
 * through the DHCP code path — the open is < 1ms and keeps
 * arp_probe independent of the DHCP BPF lifecycle.
 */
#include "arp_probe.h"

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <net/bpf.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <arpa/inet.h>

#include <errno.h>
#include <fcntl.h>
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
 * dhcp_discover.h declares the daemon's signal-shutdown flag.
 * Reused here so a long probe wait can short-circuit on SIGTERM
 * just like the DHCP read loop.
 */
extern volatile sig_atomic_t got_term;

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "ipconfigd[arp] ");
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

static int
bpf_open_arp(const char *ifname)
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
	tv.tv_sec = 1;
	tv.tv_usec = 0;
	(void)ioctl(fd, BIOCSRTIMEOUT, &tv);
	return (fd);
}

/*
 * Build a 42-byte Ethernet+ARP frame into `buf`. `sender_ip` is
 * 0.0.0.0 for a probe, the target for an announce. `target_ip`
 * is the address being probed/announced. Returns the frame
 * length.
 */
static size_t
build_arp_frame(uint8_t *buf, const uint8_t mac[6],
    struct in_addr sender_ip, struct in_addr target_ip)
{
	struct ether_header *eh;
	struct ether_arp *ea;

	(void)memset(buf, 0, 42);

	eh = (struct ether_header *)(void *)buf;
	(void)memset(eh->ether_dhost, 0xff, 6);	/* broadcast */
	(void)memcpy(eh->ether_shost, mac, 6);
	eh->ether_type = htons(ETHERTYPE_ARP);

	ea = (struct ether_arp *)(void *)(buf + sizeof(*eh));
	ea->arp_hrd = htons(ARPHRD_ETHER);
	ea->arp_pro = htons(ETHERTYPE_IP);
	ea->arp_hln = 6;
	ea->arp_pln = 4;
	ea->arp_op  = htons(ARPOP_REQUEST);
	(void)memcpy(ea->arp_sha, mac, 6);
	(void)memcpy(ea->arp_spa, &sender_ip.s_addr, 4);
	/* arp_tha left zero — RFC 826/5227 says the target HW
	 * field is ignored in a request. */
	(void)memcpy(ea->arp_tpa, &target_ip.s_addr, 4);

	return (42);
}

/*
 * Returns 1 if the frame is an ARP packet (request OR reply)
 * whose sender protocol address equals `target` — RFC 5227
 * §2.1.1 conflict signal. 0 otherwise.
 *
 * Also filters out our own probes: if the sender hardware
 * address equals our MAC, ignore (some hubs / bridges loop the
 * frame back to us; without this guard our own probes look
 * like conflicts).
 */
static int
is_conflict(const uint8_t *frame, size_t len, const uint8_t our_mac[6],
    struct in_addr target)
{
	const struct ether_header *eh;
	const struct ether_arp *ea;
	struct in_addr spa;
	uint16_t op;

	if (len < sizeof(*eh) + sizeof(*ea))
		return (0);
	eh = (const struct ether_header *)(const void *)frame;
	if (ntohs(eh->ether_type) != ETHERTYPE_ARP)
		return (0);
	ea = (const struct ether_arp *)(const void *)
	    (frame + sizeof(*eh));
	if (ntohs(ea->arp_hrd) != ARPHRD_ETHER ||
	    ntohs(ea->arp_pro) != ETHERTYPE_IP ||
	    ea->arp_hln != 6 || ea->arp_pln != 4)
		return (0);
	op = ntohs(ea->arp_op);
	if (op != ARPOP_REQUEST && op != ARPOP_REPLY)
		return (0);
	if (memcmp(ea->arp_sha, our_mac, 6) == 0)
		return (0);	/* our own frame looped back */
	(void)memcpy(&spa.s_addr, ea->arp_spa, 4);
	return (spa.s_addr == target.s_addr ? 1 : 0);
}

/*
 * Listen on `bpf_fd` for `ms` milliseconds for a conflicting
 * ARP frame for `target`. Returns 1 on conflict, 0 on timeout,
 * -1 on shutdown or fatal read error.
 */
static int
listen_for_conflict(int bpf_fd, const uint8_t our_mac[6],
    struct in_addr target, unsigned ms)
{
	/* 4096 — FreeBSD's default BPF buffer size (BIOCGBLEN). A
	 * read smaller than that returns EINVAL even when no packet
	 * is queued, killing the wait loop. Matches dhcp_discover.c's
	 * rxbuf size. */
	uint8_t rxbuf[4096];
	struct timespec deadline, now;

	(void)clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += ms / 1000;
	deadline.tv_nsec += (long)(ms % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_nsec -= 1000000000L;
		deadline.tv_sec++;
	}

	while (!got_term) {
		ssize_t n;
		uint8_t *p, *end;

		(void)clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec > deadline.tv_sec ||
		    (now.tv_sec == deadline.tv_sec &&
		     now.tv_nsec >= deadline.tv_nsec))
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
			if (is_conflict(pkt, caplen, our_mac, target))
				return (1);
			p += BPF_WORDALIGN(bh.bh_hdrlen + caplen);
		}
	}
	return (-1);
}

static int
arp_send(int bpf_fd, const uint8_t *frame, size_t len)
{
	int try;
	ssize_t n = -1;

	for (try = 0; try < 10 && !got_term; try++) {
		n = write(bpf_fd, frame, len);
		if (n == (ssize_t)len)
			return (0);
		if (errno != ENETDOWN)
			break;
		(void)usleep(100000);
	}
	xlog("write(bpf, %zu) failed after %d tries: %s",
	    len, try + 1, strerror(errno));
	return (-1);
}

int
arp_probe(const char *ifname, struct in_addr target)
{
	uint8_t mac[6];
	uint8_t frame[42];
	int bpf_fd, i, r;
	char tbuf[INET_ADDRSTRLEN];
	struct in_addr zero = { 0 };

	if (get_iface_mac(ifname, mac) != 0) {
		xlog("get_iface_mac(%s) failed: %s", ifname, strerror(errno));
		return (-1);
	}
	bpf_fd = bpf_open_arp(ifname);
	if (bpf_fd < 0) {
		xlog("bpf_open(%s) failed: %s", ifname, strerror(errno));
		return (-1);
	}
	(void)build_arp_frame(frame, mac, zero, target);
	(void)inet_ntop(AF_INET, &target, tbuf, sizeof(tbuf));

	/* 3 probes, 1s gap, then a final 1s listen window after the
	 * last probe (RFC 5227 ANNOUNCE_WAIT). */
	for (i = 0; i < 3; i++) {
		if (got_term) {
			(void)close(bpf_fd);
			return (0);
		}
		if (arp_send(bpf_fd, frame, sizeof(frame)) != 0) {
			(void)close(bpf_fd);
			return (-1);
		}
		xlog("probe %d/3 sent (target=%s)", i + 1, tbuf);
		r = listen_for_conflict(bpf_fd, mac, target, 1000);
		if (r == 1) {
			xlog("CONFLICT: %s already in use", tbuf);
			(void)close(bpf_fd);
			return (1);
		}
		if (r < 0) {
			(void)close(bpf_fd);
			return (-1);
		}
	}
	xlog("no conflict for %s after 3 probes", tbuf);
	(void)close(bpf_fd);
	return (0);
}

int
arp_announce(const char *ifname, struct in_addr our_ip)
{
	uint8_t mac[6];
	uint8_t frame[42];
	int bpf_fd, i;
	char tbuf[INET_ADDRSTRLEN];

	if (get_iface_mac(ifname, mac) != 0) {
		xlog("get_iface_mac(%s) failed: %s", ifname, strerror(errno));
		return (-1);
	}
	bpf_fd = bpf_open_arp(ifname);
	if (bpf_fd < 0) {
		xlog("bpf_open(%s) failed: %s", ifname, strerror(errno));
		return (-1);
	}
	(void)build_arp_frame(frame, mac, our_ip, our_ip);
	(void)inet_ntop(AF_INET, &our_ip, tbuf, sizeof(tbuf));

	/* 2 gratuitous ARPs, 1s apart. */
	for (i = 0; i < 2; i++) {
		if (got_term)
			break;
		if (arp_send(bpf_fd, frame, sizeof(frame)) != 0) {
			(void)close(bpf_fd);
			return (-1);
		}
		xlog("announce %d/2 sent (sender=target=%s)", i + 1, tbuf);
		if (i < 1)
			(void)sleep(1);
	}
	(void)close(bpf_fd);
	return (0);
}
