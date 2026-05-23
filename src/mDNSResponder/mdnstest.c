/*
 * mdnstest — iter 1 liveness probe for mDNSResponder.
 *
 * bootstrap_look_up the service, print MDNS-BOOT-OK on success.
 * Same shape as ipconfigtest / hwregtest. run.sh runs it and the
 * marker gates in tests/boot-test.sh.
 */
#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <stdio.h>

int
main(void)
{
	mach_port_t svc = MACH_PORT_NULL;
	kern_return_t kr;

	kr = bootstrap_look_up(bootstrap_port,
	    "com.apple.mDNSResponder", &svc);
	if (kr != KERN_SUCCESS || svc == MACH_PORT_NULL) {
		(void)printf("MDNS-BOOT-FAIL: bootstrap_look_up: 0x%x\n",
		    (unsigned)kr);
		return (1);
	}
	(void)printf("MDNS-BOOT-OK: com.apple.mDNSResponder reachable "
	    "(send right=0x%x)\n", (unsigned)svc);
	return (0);
}
