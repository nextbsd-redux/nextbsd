/*
 * sshd-mdns-register — advertise this host's sshd over Bonjour (_ssh._tcp).
 *
 * Why this exists: mDNSResponder publishes the host's <name>.local
 * address record only once at least one service that targets the local
 * hostname is registered — mDNSCore's AdvertiseInterfaceIfNeeded() gates
 * the per-interface A/PTR records on m->AutoTargetServices > 0. A
 * headless box with no registered services advertises nothing, so it
 * can't be reached by <name>.local at all (its Auth Records list is
 * empty). Registering _ssh._tcp here bumps AutoTargetServices, which
 *   (a) makes mDNSResponder advertise the host A/PTR records, so
 *       <name>.local resolves, and
 *   (b) makes the box discoverable as an SSH service in Bonjour
 *       browsers (alongside other _ssh._tcp hosts on the LAN).
 *
 * Lifecycle: sshd-keygen-wrapper starts this in the background right
 * before exec'ing `sshd -D`, so our parent becomes the sshd process.
 * We pump the registration socket and watch our parent; when sshd goes
 * away we are reparented to launchd (PID 1), and getppid() == 1 is our
 * cue to deregister and exit. Deregistering drops AutoTargetServices
 * back toward zero, so the host's .local advertisement is tied to sshd
 * being up — matching the rescue-daemon design (see com.openssh.sshd
 * .plist). .home.local / the bare hostname do NOT depend on this: those
 * resolve via the router's DNS off the DHCP host-name option (see
 * ipconfigd / dhcp_discover.c), independent of mDNS and sshd.
 *
 * Apple's launchd advertises _ssh._tcp via the ssh plist's Bonjour key;
 * this image's launchd has no Bonjour key, so we register from a tiny
 * co-process using the same libdns_sd client API instead.
 */
#include "dns_sd.h"

#include <arpa/inet.h>		/* htons */

#include <signal.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#define SSH_SVC_TYPE	"_ssh._tcp"
#define SSH_SVC_PORT	22
#define PARENT_POLL_SEC	2

static volatile sig_atomic_t g_stop;

static void
on_term(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void DNSSD_API
register_cb(DNSServiceRef sdRef, DNSServiceFlags flags,
    DNSServiceErrorType errorCode, const char *name, const char *regtype,
    const char *domain, void *context)
{
	(void)sdRef;
	(void)flags;
	(void)context;
	(void)fprintf(stderr,
	    "sshd-mdns-register: register cb err=%d name=%s type=%s domain=%s\n",
	    (int)errorCode, name ? name : "?", regtype ? regtype : "?",
	    domain ? domain : "?");
	(void)fflush(stderr);
}

int
main(void)
{
	DNSServiceRef ref = NULL;
	DNSServiceErrorType err;
	int fd;

	(void)signal(SIGTERM, on_term);
	(void)signal(SIGINT, on_term);

	/*
	 * name=NULL -> mDNSResponder uses the host's .local name; host=NULL
	 * -> the service targets the daemon's own hostname (AutoTarget).
	 * That AutoTarget SRV is what bumps AutoTargetServices and triggers
	 * host-record advertisement.
	 *
	 * Retry until it succeeds: mDNSResponder and sshd are both
	 * RunAtLoad, and if sshd wins the boot race the daemon's
	 * /var/run/mDNSResponder socket may not exist yet, so the register
	 * (or the implicit connection) fails. We are not launchd-managed —
	 * sshd-keygen-wrapper backgrounded us — so nothing would restart us
	 * until sshd itself respawned. Loop instead, giving up only if sshd
	 * (our parent) goes away (getppid()==1) or we are signalled. */
	for (;;) {
		err = DNSServiceRegister(&ref, 0, 0,
		    NULL,		/* name = host's .local name */
		    SSH_SVC_TYPE,
		    NULL,		/* domain = ".local." */
		    NULL,		/* host = this host (AutoTarget) */
		    htons(SSH_SVC_PORT),
		    0, NULL,		/* txt record */
		    register_cb, NULL);
		if (err == kDNSServiceErr_NoError)
			break;
		if (g_stop || getppid() == 1) {
			(void)fprintf(stderr, "sshd-mdns-register: giving up "
			    "(register err=%d, sshd gone/stopped)\n", (int)err);
			return (1);
		}
		(void)fprintf(stderr, "sshd-mdns-register: DNSServiceRegister "
		    "failed (err=%d) — mDNSResponder not up yet? retrying\n",
		    (int)err);
		(void)fflush(stderr);
		(void)sleep(PARENT_POLL_SEC);
	}

	fd = DNSServiceRefSockFD(ref);
	if (fd < 0) {
		(void)fprintf(stderr, "sshd-mdns-register: DNSServiceRefSockFD "
		    "failed\n");
		DNSServiceRefDeallocate(ref);
		return (1);
	}

	(void)fprintf(stderr, "sshd-mdns-register: advertising " SSH_SVC_TYPE
	    " on port %d (tied to sshd pid %ld)\n", SSH_SVC_PORT,
	    (long)getppid());
	(void)fflush(stderr);

	/*
	 * Pump the registration socket and watch our parent. We were
	 * backgrounded by sshd-keygen-wrapper just before it exec'd sshd,
	 * so getppid() is the sshd pid. When sshd exits we are reparented
	 * to launchd (PID 1); getppid() == 1 is the signal to deregister
	 * and exit so the _ssh._tcp advertisement (and the host .local
	 * record it carries) goes away with sshd.
	 */
	while (!g_stop && getppid() != 1) {
		fd_set rfds;
		struct timeval tv;
		int r;

		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		tv.tv_sec = PARENT_POLL_SEC;
		tv.tv_usec = 0;
		r = select(fd + 1, &rfds, NULL, NULL, &tv);
		if (r > 0 && FD_ISSET(fd, &rfds))
			(void)DNSServiceProcessResult(ref);
	}

	(void)fprintf(stderr, "sshd-mdns-register: sshd gone (getppid=%ld) — "
	    "deregistering " SSH_SVC_TYPE "\n", (long)getppid());
	(void)fflush(stderr);
	DNSServiceRefDeallocate(ref);
	return (0);
}
