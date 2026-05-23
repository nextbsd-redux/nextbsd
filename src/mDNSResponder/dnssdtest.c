/*
 * dnssdtest — iter 3 end-to-end DNS-SD round-trip probe for mDNSResponder.
 *
 * Uses libdns_sd to register a synthetic service ("freebsd-launchd-mach-
 * iter3" of type "_iter3._tcp") and then browse for it. The Register
 * goes through libdns_sd's AF_UNIX wire protocol to the daemon at
 * /var/run/mDNSResponder; the daemon registers the record in its
 * authoritative zone and announces it over multicast. The Browse
 * goes through the same socket; the daemon's local-record cache
 * returns our own registration. End-to-end proof that the iter-2
 * engine + libdns_sd client + uds_daemon pipe all work.
 *
 * Marker: MDNS-DNSSD-OK on round-trip success, MDNS-DNSSD-FAIL on
 * any libdns_sd error or 5s timeout. run.sh runs this after the
 * MDNS-ENGINE-OK marker fires; boot-test.sh gates on the marker.
 */
#include "dns_sd.h"

#include <arpa/inet.h>		/* htons */

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define DNSSD_SVC_NAME		"freebsd-launchd-mach-iter3"
#define DNSSD_SVC_TYPE		"_iter3._tcp"
#define DNSSD_SVC_PORT		12345
#define DNSSD_BROWSE_TIMEOUT	5	/* seconds */

static volatile int got_browse_hit;

static void DNSSD_API
register_cb(DNSServiceRef sdRef, DNSServiceFlags flags,
    DNSServiceErrorType errorCode, const char *name, const char *regtype,
    const char *domain, void *context)
{
	(void)sdRef;
	(void)flags;
	(void)context;
	(void)fprintf(stderr,
	    "dnssdtest: register cb err=%d name=%s type=%s domain=%s\n",
	    (int)errorCode, name ? name : "?",
	    regtype ? regtype : "?", domain ? domain : "?");
}

static void DNSSD_API
browse_cb(DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t ifIdx,
    DNSServiceErrorType err, const char *serviceName, const char *regtype,
    const char *domain, void *context)
{
	(void)sdRef;
	(void)context;
	(void)fprintf(stderr,
	    "dnssdtest: browse cb err=%d flags=0x%x ifidx=%u name=%s "
	    "type=%s domain=%s\n",
	    (int)err, (unsigned)flags, (unsigned)ifIdx,
	    serviceName ? serviceName : "?",
	    regtype ? regtype : "?", domain ? domain : "?");
	if (err == kDNSServiceErr_NoError && serviceName != NULL &&
	    strcmp(serviceName, DNSSD_SVC_NAME) == 0)
		got_browse_hit = 1;
}

int
main(void)
{
	DNSServiceRef reg_ref = NULL;
	DNSServiceRef browse_ref = NULL;
	DNSServiceErrorType err;
	int reg_fd, browse_fd, max_fd;
	time_t deadline;

	err = DNSServiceRegister(&reg_ref, 0, 0,
	    DNSSD_SVC_NAME, DNSSD_SVC_TYPE,
	    NULL,			/* domain (= ".local.") */
	    NULL,			/* host (= this host) */
	    htons(DNSSD_SVC_PORT),
	    0, NULL,			/* txt record */
	    register_cb, NULL);
	if (err != kDNSServiceErr_NoError) {
		(void)printf("MDNS-DNSSD-FAIL: DNSServiceRegister returned "
		    "%d\n", (int)err);
		return (1);
	}

	err = DNSServiceBrowse(&browse_ref, 0, 0,
	    DNSSD_SVC_TYPE, NULL,	/* domain (= ".local.") */
	    browse_cb, NULL);
	if (err != kDNSServiceErr_NoError) {
		(void)printf("MDNS-DNSSD-FAIL: DNSServiceBrowse returned "
		    "%d\n", (int)err);
		DNSServiceRefDeallocate(reg_ref);
		return (1);
	}

	reg_fd = DNSServiceRefSockFD(reg_ref);
	browse_fd = DNSServiceRefSockFD(browse_ref);
	if (reg_fd < 0 || browse_fd < 0) {
		(void)printf("MDNS-DNSSD-FAIL: DNSServiceRefSockFD returned "
		    "%d / %d\n", reg_fd, browse_fd);
		DNSServiceRefDeallocate(browse_ref);
		DNSServiceRefDeallocate(reg_ref);
		return (1);
	}
	max_fd = reg_fd > browse_fd ? reg_fd : browse_fd;

	/*
	 * Drain both DNSServiceRef sockets until the browse callback sees
	 * our own registration, or 5s elapses. Within the same daemon
	 * the browse-of-own-registration is a local-cache lookup, so it
	 * returns within ms in practice — the 5s budget is for boot
	 * race when the daemon is still wiring up the engine.
	 */
	deadline = time(NULL) + DNSSD_BROWSE_TIMEOUT;
	while (!got_browse_hit && time(NULL) < deadline) {
		fd_set rfds;
		struct timeval tv;
		int r;

		FD_ZERO(&rfds);
		FD_SET(reg_fd, &rfds);
		FD_SET(browse_fd, &rfds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		r = select(max_fd + 1, &rfds, NULL, NULL, &tv);
		if (r > 0) {
			if (FD_ISSET(reg_fd, &rfds))
				(void)DNSServiceProcessResult(reg_ref);
			if (FD_ISSET(browse_fd, &rfds))
				(void)DNSServiceProcessResult(browse_ref);
		}
	}

	if (got_browse_hit)
		(void)printf("MDNS-DNSSD-OK: registered + browsed "
		    DNSSD_SVC_TYPE " / " DNSSD_SVC_NAME "\n");
	else
		(void)printf("MDNS-DNSSD-FAIL: browse did not see our own "
		    "registration within %ds\n", DNSSD_BROWSE_TIMEOUT);

	DNSServiceRefDeallocate(browse_ref);
	DNSServiceRefDeallocate(reg_ref);
	return (got_browse_hit ? 0 : 1);
}
