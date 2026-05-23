/*
 * datest — iter 1 liveness probe for diskarbitrationd.
 *
 * bootstrap_look_up the service; print DA-BOOT-OK on success. Same
 * shape as ipconfigtest / hwregtest / mdnstest. run.sh runs it and
 * the marker gates in tests/boot-test.sh.
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
	    "com.apple.DiskArbitration", &svc);
	if (kr != KERN_SUCCESS || svc == MACH_PORT_NULL) {
		(void)printf("DA-BOOT-FAIL: bootstrap_look_up: 0x%x\n",
		    (unsigned)kr);
		return (1);
	}
	(void)printf("DA-BOOT-OK: com.apple.DiskArbitration reachable "
	    "(send right=0x%x)\n", (unsigned)svc);
	return (0);
}
