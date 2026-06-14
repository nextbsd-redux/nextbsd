/*
 * hostnamedbonjourset — CI fixture helper for hostnamed iter 5 (#156).
 *
 * Usage:  hostnamedbonjourset <fixture-hostname>
 *
 * Forces a Bonjour host-name collision so the hostnamed iter-5
 * conflict-rename feedback path can be exercised end to end.
 *
 *   Registers an AUTHORITATIVE, UNIQUE A record for <fixture>.local over
 *   multicast via libdns_sd:
 *
 *       <fixture>.local.   A   192.0.2.1      (TEST-NET-1, RFC 5737)
 *
 *   The address is deliberately a documentation-range address that will
 *   NOT match any real interface address mDNSResponder would announce, so
 *   the records genuinely conflict (identical rdata would coalesce, not
 *   conflict). The fixture wins its own probe first (nobody else owns the
 *   name yet) and reaches the announced state.
 *
 *   The test then sets SCPrefs ComputerName=<fixture> (via hostnameprefset).
 *   prefs_monitor republishes Setup:/Network/HostNames, mDNSConfigStore
 *   re-adopts <fixture> and re-probes <fixture>.local — which this fixture
 *   already owns — so mDNSCore renames the daemon's host label to
 *   <fixture>-2 and PosixDaemon.c publishes it to State:/Network/HostNames.
 *   hostnamed's observer then persists <fixture>-2 to SCPrefs ComputerName.
 *
 * Foreground — run.sh backgrounds with `&` and kills via the printed pid.
 * Prints "BONJOURSET-READY:" once the register callback fires (the fixture
 * owns the name) so run.sh can sequence without a fixed sleep.
 *
 * Not shipped to real images (CI-only); lives under
 * /usr/tests/freebsd-launchd-mach/ alongside hostnametest /
 * hostnameprefset / hostnamedhcpset / hostnamedmdnsset.
 *
 * Issue: hostnamed iter 5 — Bonjour conflict-rename feedback (#156).
 */

#include <dns_sd.h>

#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#include <netinet/in.h>
#include <arpa/inet.h>

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
		    "hostnamedbonjourset: register callback err=%d\n",
		    (int)err);
}

int
main(int argc, char **argv)
{
	DNSServiceRef sd_ref = NULL;
	DNSRecordRef rec_ref = NULL;
	DNSServiceErrorType derr;
	char fqdn[80];
	struct in_addr addr;
	int sock_fd;
	time_t deadline;

	if (argc != 2 || argv[1][0] == '\0' || strlen(argv[1]) > 63) {
		(void)fprintf(stderr,
		    "usage: %s <fixture-hostname>\n", argv[0]);
		return (2);
	}
	(void)signal(SIGTERM, on_term);
	(void)signal(SIGINT, on_term);

	/* DNSServiceRegisterRecord takes the record name as a (dotted) C
	 * string, not DNS wire format — the A record's rdata is the raw 4-byte
	 * address. */
	(void)snprintf(fqdn, sizeof(fqdn), "%s.local", argv[1]);

	/* 192.0.2.1 (RFC 5737 TEST-NET-1) — guaranteed to differ from any
	 * real interface address mDNSResponder announces, so the A records
	 * conflict instead of coalescing. */
	if (inet_pton(AF_INET, "192.0.2.1", &addr) != 1) {
		(void)fprintf(stderr, "BONJOURSET-FAIL: inet_pton\n");
		return (1);
	}

	derr = DNSServiceCreateConnection(&sd_ref);
	if (derr != kDNSServiceErr_NoError) {
		(void)fprintf(stderr,
		    "BONJOURSET-FAIL: DNSServiceCreateConnection=%d\n",
		    (int)derr);
		return (1);
	}

	/* kDNSServiceFlagsUnique: claim sole ownership of <fixture>.local A —
	 * this is what makes the daemon's later probe for the same name
	 * conflict (and rename). Forced multicast keeps it on the .local
	 * link even with a unicast resolver configured. */
	derr = DNSServiceRegisterRecord(sd_ref, &rec_ref,
	    kDNSServiceFlagsUnique | kDNSServiceFlagsForceMulticast,
	    0 /* any interface */, fqdn,
	    kDNSServiceType_A, kDNSServiceClass_IN,
	    (uint16_t)sizeof(addr.s_addr), &addr.s_addr, 120 /* ttl */,
	    register_cb, NULL);
	if (derr != kDNSServiceErr_NoError) {
		(void)fprintf(stderr,
		    "BONJOURSET-FAIL: DNSServiceRegisterRecord=%d\n",
		    (int)derr);
		DNSServiceRefDeallocate(sd_ref);
		return (1);
	}
	sock_fd = DNSServiceRefSockFD(sd_ref);
	if (sock_fd < 0) {
		(void)fprintf(stderr,
		    "BONJOURSET-FAIL: DNSServiceRefSockFD=%d\n", sock_fd);
		DNSServiceRefDeallocate(sd_ref);
		return (1);
	}

	(void)fprintf(stderr,
	    "hostnamedbonjourset: claiming %s.local A 192.0.2.1 "
	    "(pid=%d)\n", argv[1], (int)getpid());

	/* Drive the libdns_sd socket until the register callback fires — that
	 * is when mDNSResponder has accepted (probed + announced) our unique
	 * record and we own the name. */
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
		    "BONJOURSET-FAIL: register callback timeout (%ds)\n",
		    REGISTER_TIMEOUT);
		DNSServiceRefDeallocate(sd_ref);
		return (1);
	}
	if (!register_ok) {
		DNSServiceRefDeallocate(sd_ref);
		return (1);
	}

	(void)printf("BONJOURSET-READY: own %s.local (pid=%d)\n",
	    argv[1], (int)getpid());
	(void)fflush(stdout);

	/* Hold the registration alive so the daemon's probe collides with it.
	 * SIGTERM from run.sh exits via on_term -> _exit(0). */
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
