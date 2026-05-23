/*
 * ra_listen.h — IPv6 Router Advertisement listener for iter 7a.
 *
 * Single-shot: send one ICMPv6 Router Solicitation to ff02::2 on
 * `ifname`, wait up to `timeout_ms` for a Router Advertisement, parse
 * the first Prefix Information Option whose on-link + autonomous
 * flags are set, and return it.
 *
 * RFC 4861 §4.2 (RA format) + RFC 4862 §5.5.3 (SLAAC processing).
 */
#ifndef RA_LISTEN_H
#define RA_LISTEN_H

#include <netinet/in.h>
#include <stdint.h>

struct ra_info {
	struct in6_addr	prefix;			/* /N from PIO */
	uint8_t		prefix_len;
	uint32_t	valid_lifetime;		/* seconds, RFC 4861 */
	uint32_t	preferred_lifetime;	/* seconds */
	struct in6_addr	router_lladdr;		/* fe80::… source of the RA */
	uint32_t	router_scope_id;	/* ifindex for the LL gateway */
};

/*
 * Acquire SLAAC parameters on `ifname`. Returns 0 on success (out
 * populated), 1 on no-RA-within-timeout (out untouched), -1 on
 * fatal error.
 */
int ra_acquire(const char *ifname, unsigned timeout_ms, struct ra_info *out);

#endif /* RA_LISTEN_H */
