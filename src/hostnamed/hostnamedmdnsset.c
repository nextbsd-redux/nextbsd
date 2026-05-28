/*
 * hostnamedmdnsset — CI fixture helper for hostnamed iter 3b.
 *
 * Usage:  hostnamedmdnsset <fixture-hostname>
 *
 * Publishes an mDNS PTR record so the next hostnamed run exercises
 * its iter-3b Tier-3b try_mdns() path:
 *
 *   Discovers our bound IPv4 via State:/Network/Service/<UUID>/IPv4
 *   (the Addresses array element 0 — same key ipconfigd publishes per
 *   src/IPConfiguration/sc_publish.c:199-220), builds the reverse
 *   in-addr.arpa name, and registers a PTR record over mDNS via
 *   libdns_sd:
 *
 *     15.2.0.10.in-addr.arpa.   PTR   <fixture>.local.
 *
 *   When hostnamed runs, its try_mdns() issues a forced-multicast PTR
 *   query for the same reverse name. The local mDNSResponder answers
 *   from this registration; hostnamed extracts the first label
 *   ("<fixture>") and adopts it as the hostname.
 *
 * Foreground — run.sh backgrounds with `&` and kills via the printed
 * pid. Prints "MDNSSET-READY:" once the register callback fires so
 * run.sh can sequence without a fixed sleep.
 *
 * Not shipped to real images (CI-only); lives under
 * /usr/tests/freebsd-launchd-mach/ alongside hostnametest /
 * hostnameprefset / hostnamedhcpset.
 *
 * Issue: hostnamed iter 3b — mDNS PTR tier.
 */

#include <SystemConfiguration/SCDynamicStore.h>
#include <CoreFoundation/CoreFoundation.h>

#include <dns_sd.h>

#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>

#include <netinet/in.h>

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define REGISTER_TIMEOUT	5	/* seconds to wait for register cb */

static volatile sig_atomic_t got_register_cb;
static volatile sig_atomic_t register_ok;

static void
on_term(int sig)
{
	(void)sig;
	_exit(0);
}

/* Discover the bound IPv4 via SCDynamicStore — same iteration shape
 * hostnamed's try_mdns + iter-3a's try_dhcp use. */
static int
pick_ipv4(char *out, size_t outsz)
{
	SCDynamicStoreRef store = NULL;
	CFStringRef session = NULL, pattern = NULL, k_addresses = NULL;
	CFArrayRef keys = NULL;
	CFIndex i, n;
	int rc = 0;

	session = CFStringCreateWithCString(NULL, "hostnamedmdnsset",
	    kCFStringEncodingUTF8);
	pattern = CFStringCreateWithCString(NULL,
	    "State:/Network/Service/[^/]+/IPv4", kCFStringEncodingUTF8);
	k_addresses = CFStringCreateWithCString(NULL, "Addresses",
	    kCFStringEncodingUTF8);
	if (session == NULL || pattern == NULL || k_addresses == NULL)
		goto out;

	store = SCDynamicStoreCreate(NULL, session, NULL, NULL);
	if (store == NULL) {
		(void)fprintf(stderr, "SCDynamicStoreCreate failed\n");
		goto out;
	}

	keys = SCDynamicStoreCopyKeyList(store, pattern);
	if (keys == NULL || (n = CFArrayGetCount(keys)) == 0) {
		(void)fprintf(stderr,
		    "no State:/Network/Service/<UUID>/IPv4 key found "
		    "(is ipconfigd bound?)\n");
		goto out;
	}
	for (i = 0; i < n; i++) {
		CFStringRef key;
		CFPropertyListRef plist;
		CFArrayRef addrs;
		CFStringRef addr0;
		char buf[INET_ADDRSTRLEN];

		key = (CFStringRef)CFArrayGetValueAtIndex(keys, i);
		if (key == NULL)
			continue;
		plist = SCDynamicStoreCopyValue(store, key);
		if (plist == NULL)
			continue;
		if (CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
			CFRelease(plist);
			continue;
		}
		addrs = CFDictionaryGetValue((CFDictionaryRef)plist,
		    k_addresses);
		if (addrs == NULL ||
		    CFGetTypeID(addrs) != CFArrayGetTypeID() ||
		    CFArrayGetCount(addrs) == 0) {
			CFRelease(plist);
			continue;
		}
		addr0 = (CFStringRef)CFArrayGetValueAtIndex(addrs, 0);
		if (addr0 == NULL ||
		    CFGetTypeID(addr0) != CFStringGetTypeID() ||
		    !CFStringGetCString(addr0, buf, sizeof(buf),
		    kCFStringEncodingUTF8)) {
			CFRelease(plist);
			continue;
		}
		CFRelease(plist);
		if (strncmp(buf, "127.", 4) == 0 ||
		    strcmp(buf, "0.0.0.0") == 0 ||
		    strncmp(buf, "169.254.", 8) == 0)
			continue;
		(void)strncpy(out, buf, outsz - 1);
		out[outsz - 1] = '\0';
		rc = 1;
		break;
	}
out:
	if (keys != NULL) CFRelease(keys);
	if (store != NULL) CFRelease(store);
	if (k_addresses != NULL) CFRelease(k_addresses);
	if (pattern != NULL) CFRelease(pattern);
	if (session != NULL) CFRelease(session);
	return (rc);
}

static int
build_reverse_inaddr(const char *ipv4, char *out, size_t outsz)
{
	unsigned a, b, c, d;
	int n;

	if (sscanf(ipv4, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
		return (0);
	if (a > 255 || b > 255 || c > 255 || d > 255)
		return (0);
	n = snprintf(out, outsz, "%u.%u.%u.%u.in-addr.arpa", d, c, b, a);
	return (n > 0 && (size_t)n < outsz);
}

/* Encode "<label>.local" as DNS wire-format length-prefixed labels
 * with a terminating 0. Returns wire length, or 0 on overflow. */
static size_t
encode_local_name(const char *label, uint8_t *out, size_t outsz)
{
	size_t llen = strlen(label);
	size_t need = 1 + llen + 1 + 5 + 1;	/* len+label + 5+"local" + 0 */

	if (llen == 0 || llen > 63 || need > outsz)
		return (0);
	out[0] = (uint8_t)llen;
	(void)memcpy(out + 1, label, llen);
	out[1 + llen] = 5;
	(void)memcpy(out + 2 + llen, "local", 5);
	out[1 + llen + 1 + 5] = 0;
	return (need);
}

static void DNSSD_API
register_cb(DNSServiceRef sdRef, DNSRecordRef recRef, DNSServiceFlags flags,
    DNSServiceErrorType err, void *context)
{
	(void)sdRef;
	(void)recRef;
	(void)flags;
	(void)context;
	got_register_cb = 1;
	register_ok = (err == kDNSServiceErr_NoError);
	if (err != kDNSServiceErr_NoError)
		(void)fprintf(stderr,
		    "hostnamedmdnsset: register callback err=%d\n",
		    (int)err);
}

int
main(int argc, char **argv)
{
	DNSServiceRef sd_ref = NULL;
	DNSRecordRef rec_ref = NULL;
	DNSServiceErrorType derr;
	char ipv4[INET_ADDRSTRLEN];
	char reverse[128];
	uint8_t rdata[64];
	size_t rdlen;
	int sock_fd;
	time_t deadline;

	if (argc != 2 || argv[1][0] == '\0' || strlen(argv[1]) > 63) {
		(void)fprintf(stderr,
		    "usage: %s <fixture-hostname>\n", argv[0]);
		return (2);
	}
	(void)signal(SIGTERM, on_term);
	(void)signal(SIGINT, on_term);

	if (!pick_ipv4(ipv4, sizeof(ipv4))) {
		(void)fprintf(stderr, "MDNSSET-FAIL: no usable IPv4\n");
		return (1);
	}
	if (!build_reverse_inaddr(ipv4, reverse, sizeof(reverse))) {
		(void)fprintf(stderr,
		    "MDNSSET-FAIL: build_reverse_inaddr('%s')\n", ipv4);
		return (1);
	}
	rdlen = encode_local_name(argv[1], rdata, sizeof(rdata));
	if (rdlen == 0) {
		(void)fprintf(stderr,
		    "MDNSSET-FAIL: encode_local_name('%s')\n", argv[1]);
		return (1);
	}

	derr = DNSServiceCreateConnection(&sd_ref);
	if (derr != kDNSServiceErr_NoError) {
		(void)fprintf(stderr,
		    "MDNSSET-FAIL: DNSServiceCreateConnection=%d\n",
		    (int)derr);
		return (1);
	}
	derr = DNSServiceRegisterRecord(sd_ref, &rec_ref,
	    kDNSServiceFlagsShared | kDNSServiceFlagsForceMulticast,
	    0 /* any interface */, reverse,
	    kDNSServiceType_PTR, kDNSServiceClass_IN,
	    (uint16_t)rdlen, rdata, 120 /* ttl */,
	    register_cb, NULL);
	if (derr != kDNSServiceErr_NoError) {
		(void)fprintf(stderr,
		    "MDNSSET-FAIL: DNSServiceRegisterRecord=%d\n",
		    (int)derr);
		DNSServiceRefDeallocate(sd_ref);
		return (1);
	}
	sock_fd = DNSServiceRefSockFD(sd_ref);
	if (sock_fd < 0) {
		(void)fprintf(stderr,
		    "MDNSSET-FAIL: DNSServiceRefSockFD=%d\n", sock_fd);
		DNSServiceRefDeallocate(sd_ref);
		return (1);
	}

	(void)fprintf(stderr,
	    "hostnamedmdnsset: registering PTR %s -> %s.local "
	    "(pid=%d, IPv4=%s)\n",
	    reverse, argv[1], (int)getpid(), ipv4);

	/* Drive the libdns_sd socket; the register callback fires the
	 * moment mDNSResponder accepts the record. Then loop forever
	 * holding the registration; SIGTERM from run.sh exits the
	 * loop via on_term -> _exit(0). */
	deadline = time(NULL) + REGISTER_TIMEOUT;
	while (!got_register_cb && time(NULL) < deadline) {
		fd_set rfds;
		struct timeval tv;
		int r;

		FD_ZERO(&rfds);
		FD_SET(sock_fd, &rfds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		r = select(sock_fd + 1, &rfds, NULL, NULL, &tv);
		if (r > 0 && FD_ISSET(sock_fd, &rfds))
			(void)DNSServiceProcessResult(sd_ref);
	}
	if (!got_register_cb) {
		(void)fprintf(stderr,
		    "MDNSSET-FAIL: register callback timeout (%ds)\n",
		    REGISTER_TIMEOUT);
		DNSServiceRefDeallocate(sd_ref);
		return (1);
	}
	if (!register_ok) {
		DNSServiceRefDeallocate(sd_ref);
		return (1);
	}

	(void)printf("MDNSSET-READY: PTR %s -> %s.local (pid=%d)\n",
	    reverse, argv[1], (int)getpid());
	(void)fflush(stdout);

	/* Hold the registration alive. mDNSResponder serves PTR answers
	 * from this connection's cache until we exit. */
	for (;;) {
		fd_set rfds;
		struct timeval tv;
		int r;

		FD_ZERO(&rfds);
		FD_SET(sock_fd, &rfds);
		tv.tv_sec = 60;
		tv.tv_usec = 0;
		r = select(sock_fd + 1, &rfds, NULL, NULL, &tv);
		if (r > 0 && FD_ISSET(sock_fd, &rfds))
			(void)DNSServiceProcessResult(sd_ref);
	}
	/* NOTREACHED */
}
