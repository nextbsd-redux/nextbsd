/*
 * mach_bridge.c — Mach service hook for mDNSResponder.
 *
 * PosixDaemon.c's main() calls mDNSResponderMachBridgeInit() right
 * after parsing argv. That function claims com.apple.mDNSResponder
 * (bootstrap_check_in) and logs MDNS-BOOT-OK — same shape ipconfigd /
 * hwregd / configd use. The mDNS engine then comes up on the main
 * thread; uds_daemon runs alongside (AF_UNIX clients via
 * /var/run/mDNSResponder).
 *
 * iter 2 ships only the receive-right claim; no MIG demux yet. iter 3
 * adds the Apple-shape dns_sd.defs MIG IDL so client libs can use
 * either AF_UNIX (legacy libdns_sd) or Mach RPC (Apple-shape
 * DNSServiceRef) interchangeably.
 */
#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <stdio.h>

int mDNSResponderMachBridgeInit(void);

int
mDNSResponderMachBridgeInit(void)
{
	mach_port_t svc = MACH_PORT_NULL;
	kern_return_t kr;

	kr = bootstrap_check_in(bootstrap_port, "com.apple.mDNSResponder",
	    &svc);
	if (kr != KERN_SUCCESS || svc == MACH_PORT_NULL) {
		(void)fprintf(stderr, "mDNSResponder MDNS-BOOT-FAIL: "
		    "bootstrap_check_in: 0x%x\n", (unsigned)kr);
		return (-1);
	}
	(void)fprintf(stderr, "mDNSResponder MDNS-BOOT-OK: "
	    "com.apple.mDNSResponder registered (receive right=0x%x)\n",
	    (unsigned)svc);
	(void)fflush(stderr);
	return (0);
}
