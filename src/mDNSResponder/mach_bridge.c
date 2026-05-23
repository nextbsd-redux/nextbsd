/*
 * mach_bridge.c — the iter-2 Mach service hook for mDNSResponder.
 *
 * PosixDaemon.c's main() calls mDNSResponderMachBridgeInit() right
 * after parsing argv. That function claims the com.apple.mDNSResponder
 * Mach service (bootstrap_check_in) and logs MDNS-BOOT-OK so the boot
 * test gates on the Mach surface coming up — same shape ipconfigd /
 * hwregd / configd use. The mDNS engine then comes up on the main
 * thread; uds_daemon also runs (AF_UNIX clients via /var/run/
 * mDNSResponder).
 *
 * iter 2 ships only the receive-right claim; no MIG demux yet. iter 3
 * grows the Apple-shape dns_sd.defs MIG IDL so client libs can use
 * either AF_UNIX (legacy libdns_sd compat) or Mach RPC (Apple-shape
 * DNSServiceRef).
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
