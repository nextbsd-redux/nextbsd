/*
 * sc_publish.c — SCDynamicStore publish glue for ipconfigd.
 *
 * Builds Apple's State:/Network/Service/<UUID>/IPv4 (+ optional /DNS)
 * CFDictionary and pushes it into configd via libSystemConfiguration.
 * The <UUID> is a deterministic 8-4-4-4-12-hex string derived from
 * the interface MAC — stable across daemon restarts even without
 * SCPreferences persistence, so a watcher's regex match stays put.
 *
 * Single CF translation unit on purpose: everything else in the
 * daemon stays plain-C / plain-Apple-headers.
 */
#include "sc_publish.h"

#include <SystemConfiguration/SCDynamicStore.h>
#include <CoreFoundation/CoreFoundation.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <errno.h>
#include <ifaddrs.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sc_publish {
	SCDynamicStoreRef	store;
};

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "ipconfigd[sc] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/* CFStringCreateWithCString wrapper — avoids -fconstant-cfstrings. */
static CFStringRef
mkstr(const char *s)
{
	return (CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8));
}

static int
get_mac(const char *ifname, uint8_t mac[6])
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
 * Format an Apple-shaped UUID string from the interface MAC. Apple's
 * IPConfiguration draws UUIDs from SCPreferences (a user-configured
 * SCNetworkService maps to a UUID); we have no prefs persistence yet,
 * so synthesize one. Format is real 8-4-4-4-12 lowercase hex so
 * downstream observers that regex-match on UUID keys work unchanged.
 *
 *   00000000-0000-0000-AABB-AABBCCDDEEFF
 *                      ^^^^ ^^^^^^^^^^^^
 *                      MAC[0..1] MAC[0..5]
 *
 * Result is heap-allocated; caller releases with CFRelease.
 */
static CFStringRef
make_service_uuid(const char *ifname)
{
	uint8_t mac[6] = { 0, 0, 0, 0, 0, 0 };
	char buf[40];

	(void)get_mac(ifname, mac);
	(void)snprintf(buf, sizeof(buf),
	    "00000000-0000-0000-%02x%02x-%02x%02x%02x%02x%02x%02x",
	    mac[0], mac[1],
	    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return (mkstr(buf));
}

static CFStringRef
make_key(const char *ifname, const char *suffix)
{
	CFStringRef uuid = make_service_uuid(ifname);
	CFStringRef key;

	key = CFStringCreateWithFormat(NULL, NULL,
	    CFSTR("State:/Network/Service/%@/%s"), uuid, suffix);
	CFRelease(uuid);
	return (key);
}

struct sc_publish *
sc_publish_open(const char *session_name)
{
	struct sc_publish *p;
	CFStringRef name;

	p = calloc(1, sizeof(*p));
	if (p == NULL)
		return (NULL);

	name = mkstr(session_name);
	p->store = SCDynamicStoreCreate(NULL, name, NULL, NULL);
	CFRelease(name);
	if (p->store == NULL) {
		xlog("SCDynamicStoreCreate failed: %s",
		    SCErrorString(SCError()));
		free(p);
		return (NULL);
	}
	return (p);
}

static void
add_addr_string(CFMutableDictionaryRef dict, CFStringRef key,
    const struct in_addr *a)
{
	char buf[INET_ADDRSTRLEN];
	CFStringRef s;

	if (inet_ntop(AF_INET, a, buf, sizeof(buf)) == NULL)
		return;
	s = mkstr(buf);
	CFDictionarySetValue(dict, key, s);
	CFRelease(s);
}

static void
add_single_addr_array(CFMutableDictionaryRef dict, CFStringRef key,
    const struct in_addr *a)
{
	char buf[INET_ADDRSTRLEN];
	CFStringRef s;
	CFArrayRef arr;
	const void *vals[1];

	if (inet_ntop(AF_INET, a, buf, sizeof(buf)) == NULL)
		return;
	s = mkstr(buf);
	vals[0] = s;
	arr = CFArrayCreate(NULL, vals, 1, &kCFTypeArrayCallBacks);
	CFDictionarySetValue(dict, key, arr);
	CFRelease(arr);
	CFRelease(s);
}

int
sc_publish_ipv4(struct sc_publish *p, const char *ifname,
    const struct dhcp_lease *lease)
{
	CFMutableDictionaryRef dict;
	CFStringRef key, k_addresses, k_masks, k_router, k_iface, k_server;
	CFStringRef iface_str;
	Boolean ok;

	if (p == NULL || p->store == NULL)
		return (-1);

	dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	if (dict == NULL)
		return (-1);

	k_addresses = mkstr("Addresses");
	k_masks = mkstr("SubnetMasks");
	k_router = mkstr("Router");
	k_iface = mkstr("InterfaceName");
	k_server = mkstr("ServerIdentifier");

	add_single_addr_array(dict, k_addresses, &lease->addr);
	add_single_addr_array(dict, k_masks, &lease->netmask);
	add_addr_string(dict, k_router, &lease->router);
	add_addr_string(dict, k_server, &lease->server);

	iface_str = mkstr(ifname);
	CFDictionarySetValue(dict, k_iface, iface_str);
	CFRelease(iface_str);

	CFRelease(k_addresses);
	CFRelease(k_masks);
	CFRelease(k_router);
	CFRelease(k_iface);
	CFRelease(k_server);

	key = make_key(ifname, "IPv4");
	ok = SCDynamicStoreSetValue(p->store, key, dict);
	CFRelease(key);
	CFRelease(dict);

	if (!ok) {
		xlog("SCDynamicStoreSetValue(IPv4) failed: %s",
		    SCErrorString(SCError()));
		return (-1);
	}

	/* DNS — optional, only if the lease carried DNS option 6. */
	if (lease->dns_count > 0) {
		CFMutableDictionaryRef dns_dict;
		CFMutableArrayRef dns_arr;
		CFStringRef k_servers;
		unsigned i;

		dns_dict = CFDictionaryCreateMutable(NULL, 0,
		    &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
		dns_arr = CFArrayCreateMutable(NULL, 0,
		    &kCFTypeArrayCallBacks);
		if (dns_dict == NULL || dns_arr == NULL) {
			if (dns_dict != NULL)
				CFRelease(dns_dict);
			if (dns_arr != NULL)
				CFRelease(dns_arr);
			return (0);	/* IPv4 publish succeeded */
		}
		for (i = 0; i < lease->dns_count; i++) {
			char buf[INET_ADDRSTRLEN];
			CFStringRef s;

			if (inet_ntop(AF_INET, &lease->dns[i], buf,
			    sizeof(buf)) == NULL)
				continue;
			s = mkstr(buf);
			CFArrayAppendValue(dns_arr, s);
			CFRelease(s);
		}
		k_servers = mkstr("ServerAddresses");
		CFDictionarySetValue(dns_dict, k_servers, dns_arr);
		CFRelease(k_servers);
		CFRelease(dns_arr);

		key = make_key(ifname, "DNS");
		ok = SCDynamicStoreSetValue(p->store, key, dns_dict);
		CFRelease(key);
		CFRelease(dns_dict);
		if (!ok)
			xlog("SCDynamicStoreSetValue(DNS) failed: %s",
			    SCErrorString(SCError()));
	}
	return (0);
}

int
sc_publish_ipv6(struct sc_publish *p, const char *ifname,
    const struct in6_addr *addr, uint8_t prefix_len,
    const struct in6_addr *router_lladdr)
{
	CFMutableDictionaryRef dict;
	CFStringRef key, k_addresses, k_prefixlen, k_router;
	CFStringRef k_iface, k_flags;
	CFStringRef iface_str, addr_str, router_str;
	CFArrayRef addr_arr, plen_arr;
	CFNumberRef plen_num, flags_num;
	const void *addr_vals[1];
	const void *plen_vals[1];
	char abuf[INET6_ADDRSTRLEN];
	char rbuf[INET6_ADDRSTRLEN + IFNAMSIZ + 2];
	int plen_i, flags_i = 0;
	Boolean ok;

	if (p == NULL || p->store == NULL)
		return (-1);

	if (inet_ntop(AF_INET6, addr, abuf, sizeof(abuf)) == NULL)
		return (-1);
	{
		char rbase[INET6_ADDRSTRLEN];

		if (inet_ntop(AF_INET6, router_lladdr, rbase,
		    sizeof(rbase)) == NULL)
			return (-1);
		(void)snprintf(rbuf, sizeof(rbuf), "%s%%%s", rbase, ifname);
	}

	dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	if (dict == NULL)
		return (-1);

	k_addresses = mkstr("Addresses");
	k_prefixlen = mkstr("PrefixLength");
	k_router = mkstr("Router");
	k_iface = mkstr("InterfaceName");
	k_flags = mkstr("Flags");

	addr_str = mkstr(abuf);
	addr_vals[0] = addr_str;
	addr_arr = CFArrayCreate(NULL, addr_vals, 1, &kCFTypeArrayCallBacks);
	CFDictionarySetValue(dict, k_addresses, addr_arr);
	CFRelease(addr_arr);
	CFRelease(addr_str);

	plen_i = prefix_len;
	plen_num = CFNumberCreate(NULL, kCFNumberIntType, &plen_i);
	plen_vals[0] = plen_num;
	plen_arr = CFArrayCreate(NULL, plen_vals, 1, &kCFTypeArrayCallBacks);
	CFDictionarySetValue(dict, k_prefixlen, plen_arr);
	CFRelease(plen_arr);
	CFRelease(plen_num);

	router_str = mkstr(rbuf);
	CFDictionarySetValue(dict, k_router, router_str);
	CFRelease(router_str);

	iface_str = mkstr(ifname);
	CFDictionarySetValue(dict, k_iface, iface_str);
	CFRelease(iface_str);

	flags_num = CFNumberCreate(NULL, kCFNumberIntType, &flags_i);
	CFDictionarySetValue(dict, k_flags, flags_num);
	CFRelease(flags_num);

	CFRelease(k_addresses);
	CFRelease(k_prefixlen);
	CFRelease(k_router);
	CFRelease(k_iface);
	CFRelease(k_flags);

	key = make_key(ifname, "IPv6");
	ok = SCDynamicStoreSetValue(p->store, key, dict);
	CFRelease(key);
	CFRelease(dict);

	if (!ok) {
		xlog("SCDynamicStoreSetValue(IPv6) failed: %s",
		    SCErrorString(SCError()));
		return (-1);
	}
	return (0);
}

int
sc_publish_remove(struct sc_publish *p, const char *ifname)
{
	CFStringRef key;
	Boolean ok = TRUE;

	if (p == NULL || p->store == NULL)
		return (-1);

	key = make_key(ifname, "IPv4");
	if (!SCDynamicStoreRemoveValue(p->store, key))
		ok = FALSE;
	CFRelease(key);

	key = make_key(ifname, "DNS");
	(void)SCDynamicStoreRemoveValue(p->store, key);
	CFRelease(key);

	key = make_key(ifname, "IPv6");
	(void)SCDynamicStoreRemoveValue(p->store, key);
	CFRelease(key);

	return (ok ? 0 : -1);
}

void
sc_publish_close(struct sc_publish *p)
{
	if (p == NULL)
		return;
	if (p->store != NULL)
		CFRelease(p->store);
	free(p);
}
