/*
 * ipconfigrpctest — iter 5a RPC liveness probe for ipconfigd.
 *
 * bootstrap_look_up the service, call ipconfig_if_count (expects
 * 1 after BOUND), call ipconfig_if_addr("em0") (expects the
 * SLIRP-assigned address, typically 10.0.2.15). Prints
 * IPCFG-RPC-OK on success; the marker gates in tests/boot-test.sh.
 *
 * Build (build.sh): cc + the MIG-generated ipconfigUser.c.
 */
#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include "ipconfig_mig_types.h"
#include "ipconfig.h"		/* MIG: ipconfig_* client stubs */

#include <stdio.h>
#include <string.h>

int
main(void)
{
	mach_port_t svc = MACH_PORT_NULL;
	kern_return_t kr;
	int count = -1;
	InterfaceName name;
	ip_address_t addr = 0;
	ipconfig_status_t status = ipconfig_status_internal_error_e;
	char abuf[INET_ADDRSTRLEN];
	struct in_addr in_a;

	kr = bootstrap_look_up(bootstrap_port,
	    "com.apple.IPConfiguration", &svc);
	if (kr != KERN_SUCCESS || svc == MACH_PORT_NULL) {
		printf("IPCFG-RPC-FAIL: bootstrap_look_up: 0x%x\n",
		    (unsigned)kr);
		return (1);
	}

	kr = ipconfig_if_count(svc, &count);
	if (kr != KERN_SUCCESS) {
		printf("IPCFG-RPC-FAIL: ipconfig_if_count returned 0x%x\n",
		    (unsigned)kr);
		return (1);
	}
	printf("  ipconfig_if_count -> %d\n", count);
	if (count < 1) {
		printf("IPCFG-RPC-FAIL: ipconfig_if_count returned %d "
		    "(expected >= 1)\n", count);
		return (1);
	}

	(void)memset(name, 0, sizeof(name));
	(void)strlcpy(name, "em0", sizeof(name));
	kr = ipconfig_if_addr(svc, name, &addr, &status);
	if (kr != KERN_SUCCESS) {
		printf("IPCFG-RPC-FAIL: ipconfig_if_addr returned 0x%x\n",
		    (unsigned)kr);
		return (1);
	}
	if (status != ipconfig_status_success_e) {
		printf("IPCFG-RPC-FAIL: ipconfig_if_addr status=%d\n",
		    (int)status);
		return (1);
	}
	in_a.s_addr = addr;
	if (inet_ntop(AF_INET, &in_a, abuf, sizeof(abuf)) == NULL)
		(void)strlcpy(abuf, "?", sizeof(abuf));
	printf("  ipconfig_if_addr(em0) -> %s\n", abuf);

	printf("IPCFG-RPC-OK: ipconfig_if_count=%d em0=%s\n", count, abuf);
	return (0);
}
