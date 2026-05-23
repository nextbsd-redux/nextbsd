/*
 * ipconfigtest — iter 1 liveness probe for ipconfigd.
 *
 * iter 1 has no Mach RPC surface yet (the config.defs MIG demux
 * lands later). The signal that the daemon launched + claimed its
 * service is that bootstrap_look_up returns a non-null send right
 * for com.apple.IPConfiguration. That is the iter-1 marker.
 *
 * Same shape as hwregtest / configtest's first-iter probes — they
 * test exactly this property of their respective daemons. Marker
 * IPCFG-BOOT-OK gates in tests/boot-test.sh.
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
	    "com.apple.IPConfiguration", &svc);
	if (kr != KERN_SUCCESS) {
		printf("IPCFG-BOOT-FAIL: bootstrap_look_up: 0x%x\n",
		    (unsigned)kr);
		return (1);
	}
	if (svc == MACH_PORT_NULL) {
		printf("IPCFG-BOOT-FAIL: service port is MACH_PORT_NULL\n");
		return (1);
	}
	printf("IPCFG-BOOT-OK: com.apple.IPConfiguration registered "
	    "(send right=0x%x)\n", (unsigned)svc);
	return (0);
}
