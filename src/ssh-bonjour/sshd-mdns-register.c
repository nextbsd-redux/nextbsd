/*
 * sshd-mdns-register — advertise this host's sshd over Bonjour, as both
 * _ssh._tcp and _sftp-ssh._tcp.
 *
 * Why this exists: mDNSResponder publishes the host's <name>.local
 * address record only once at least one service that targets the local
 * hostname is registered — mDNSCore's AdvertiseInterfaceIfNeeded() gates
 * the per-interface A/PTR records on m->AutoTargetServices > 0. A
 * headless box with no registered services advertises nothing, so it
 * can't be reached by <name>.local at all (its Auth Records list is
 * empty). Registering an _ssh._tcp service here bumps AutoTargetServices,
 * which
 *   (a) makes mDNSResponder advertise the host A/PTR records, so
 *       <name>.local resolves, and
 *   (b) makes the box discoverable as an SSH service in Bonjour
 *       browsers (alongside other _ssh._tcp hosts on the LAN).
 *
 * Why _sftp-ssh._tcp too: Network Browser.app finds hosts via _ssh._tcp,
 * but Workspace only surfaces (and connects to) SSH hosts advertised as
 * _sftp-ssh._tcp. Apple gets both types from launchd's native Bonjour
 * socket key (ssh.plist lists <array>ssh sftp-ssh</array>); this image's
 * launchd doesn't implement that key and our sshd plist isn't socket-
 * activated, so we register both types here instead. Same port (22),
 * same AutoTarget host — _sftp-ssh is the same sshd, just the service
 * name Workspace looks for.
 *
 * NOT handled here: _device-info._tcp (the model= TXT richer browsers
 * read). On Apple that is a host-level record owned by mDNSResponder
 * itself (m->DeviceInfo / UpdateDeviceInfoRecord), present whenever the
 * host advertises at all — independent of sshd. Bolting it onto this
 * sshd co-process would make it blink in/out with Remote Login, which is
 * wrong. It belongs in mDNSResponder's stubbed UpdateDeviceInfoRecord();
 * tracked separately. See nextbsd-redux/nextbsd#291.
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
#define SFTP_SVC_TYPE	"_sftp-ssh._tcp"
#define SSH_SVC_PORT	22		/* both types front the same sshd */
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

/*
 * Register one AutoTarget service on the shared connection. name=NULL ->
 * mDNSResponder uses the host's .local name; host=NULL -> the service
 * targets the daemon's own hostname (AutoTarget). That AutoTarget SRV is
 * what bumps AutoTargetServices and triggers host-record advertisement.
 * op is a *copy* of the shared ref; the ShareConnection flag tells the
 * library to reuse the shared connection's socket rather than open a new
 * one. Deallocating the shared ref later tears down every op with it.
 */
static DNSServiceErrorType
register_svc(DNSServiceRef shared, DNSServiceRef *op, const char *type)
{
	*op = shared;
	return DNSServiceRegister(op, kDNSServiceFlagsShareConnection, 0,
	    NULL,		/* name = host's .local name */
	    type,
	    NULL,		/* domain = ".local." */
	    NULL,		/* host = this host (AutoTarget) */
	    htons(SSH_SVC_PORT),
	    0, NULL,		/* txt record */
	    register_cb, NULL);
}

int
main(void)
{
	DNSServiceRef shared = NULL;	/* shared connection; owns both ops */
	DNSServiceRef ssh_op, sftp_op;	/* copies — do not deallocate */
	DNSServiceErrorType err;
	int fd;

	(void)signal(SIGTERM, on_term);
	(void)signal(SIGINT, on_term);

	/*
	 * Open one shared connection and register both _ssh._tcp and
	 * _sftp-ssh._tcp on it, so a single socket fd drives both (and a
	 * single deallocate drops both, keeping the deregister atomic).
	 *
	 * Retry the whole bundle until it succeeds: mDNSResponder and sshd
	 * are both RunAtLoad, and if sshd wins the boot race the daemon's
	 * /var/run/mDNSResponder socket may not exist yet, so the connect
	 * or either register fails. We are not launchd-managed —
	 * sshd-keygen-wrapper backgrounded us — so nothing would restart us
	 * until sshd itself respawned. Loop instead, giving up only if sshd
	 * (our parent) goes away (getppid()==1) or we are signalled. On any
	 * partial failure tear the connection down and start over, so we
	 * never end up advertising only one of the two types. */
	for (;;) {
		err = DNSServiceCreateConnection(&shared);
		if (err == kDNSServiceErr_NoError)
			err = register_svc(shared, &ssh_op, SSH_SVC_TYPE);
		if (err == kDNSServiceErr_NoError)
			err = register_svc(shared, &sftp_op, SFTP_SVC_TYPE);
		if (err == kDNSServiceErr_NoError)
			break;
		if (shared != NULL) {
			DNSServiceRefDeallocate(shared);
			shared = NULL;
		}
		if (g_stop || getppid() == 1) {
			(void)fprintf(stderr, "sshd-mdns-register: giving up "
			    "(register err=%d, sshd gone/stopped)\n", (int)err);
			return (1);
		}
		(void)fprintf(stderr, "sshd-mdns-register: register failed "
		    "(err=%d) — mDNSResponder not up yet? retrying\n", (int)err);
		(void)fflush(stderr);
		(void)sleep(PARENT_POLL_SEC);
	}

	fd = DNSServiceRefSockFD(shared);
	if (fd < 0) {
		(void)fprintf(stderr, "sshd-mdns-register: DNSServiceRefSockFD "
		    "failed\n");
		DNSServiceRefDeallocate(shared);
		return (1);
	}

	(void)fprintf(stderr, "sshd-mdns-register: advertising " SSH_SVC_TYPE
	    " + " SFTP_SVC_TYPE " on port %d (tied to sshd pid %ld)\n",
	    SSH_SVC_PORT, (long)getppid());
	(void)fflush(stderr);

	/*
	 * Pump the registration socket and watch our parent. We were
	 * backgrounded by sshd-keygen-wrapper just before it exec'd sshd,
	 * so getppid() is the sshd pid. When sshd exits we are reparented
	 * to launchd (PID 1); getppid() == 1 is the signal to deregister
	 * and exit so the SSH advertisements (and the host .local record
	 * they carry) go away with sshd.
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
			(void)DNSServiceProcessResult(shared);
	}

	(void)fprintf(stderr, "sshd-mdns-register: sshd gone (getppid=%ld) — "
	    "deregistering " SSH_SVC_TYPE " + " SFTP_SVC_TYPE "\n",
	    (long)getppid());
	(void)fflush(stderr);
	/*
	 * Deallocating the shared ref terminates the connection and both
	 * subordinate ops at once; per dns_sd.h we must NOT also deallocate
	 * ssh_op / sftp_op (their memory is freed with the parent).
	 */
	DNSServiceRefDeallocate(shared);
	return (0);
}
