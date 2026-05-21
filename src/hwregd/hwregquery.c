/*
 * hwregquery — iter 2a test client for hwregd's Mach-RPC query API.
 *
 * Looks up the org.freebsd.hwregd Mach service, then uses the MIG
 * hwreg.defs routines (hwreg_get_root / hwreg_get_children /
 * hwreg_get_node) to walk the hardware registry tree. Prints
 * HWREG-RPC-OK on success — the CI boot test (run.sh / boot-test.sh)
 * matches that marker. Built against the MIG user stub hwregUser.c.
 */
#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <stdio.h>

#include "hwreg.h"		/* MIG user-side routine prototypes + types */

#define MAXKIDS	128		/* matches array[*:128] in hwreg.defs */

static int	walked;		/* nodes successfully visited */
static int	failed;		/* set on the first RPC error */

/* Depth-first walk of the registry subtree rooted at `id`. */
static void
walk(mach_port_t svc, uint64_t id, int depth)
{
	uint64_t parent_id;
	int node_state;
	char name[32], classname[32], driver[32], path[256];
	uint64_t kids[MAXKIDS];
	mach_msg_type_number_t nkids = MAXKIDS;
	kern_return_t kr;
	unsigned int i;

	kr = hwreg_get_node(svc, id, &parent_id, &node_state,
	    name, classname, driver, path);
	if (kr != KERN_SUCCESS) {
		printf("  hwreg_get_node(%llu) failed: 0x%x\n",
		    (unsigned long long)id, (unsigned)kr);
		failed = 1;
		return;
	}
	walked++;
	/* Print only the top few levels — keep the CI log readable. */
	if (depth < 3)
		printf("  %*s[%llu] %s (%s)\n", depth * 2, "",
		    (unsigned long long)id, name, classname);

	kr = hwreg_get_children(svc, id, kids, &nkids);
	if (kr != KERN_SUCCESS) {
		printf("  hwreg_get_children(%llu) failed: 0x%x\n",
		    (unsigned long long)id, (unsigned)kr);
		failed = 1;
		return;
	}
	for (i = 0; i < nkids; i++)
		walk(svc, kids[i], depth + 1);
}

int
main(void)
{
	mach_port_t svc = MACH_PORT_NULL;
	kern_return_t kr;
	uint64_t root = 0;

	kr = bootstrap_look_up(bootstrap_port, "org.freebsd.hwregd", &svc);
	if (kr != KERN_SUCCESS) {
		printf("HWREG-RPC-FAIL: bootstrap_look_up: 0x%x\n",
		    (unsigned)kr);
		return 1;
	}

	kr = hwreg_get_root(svc, &root);
	if (kr != KERN_SUCCESS) {
		printf("HWREG-RPC-FAIL: hwreg_get_root: 0x%x\n", (unsigned)kr);
		return 1;
	}
	if (root == 0) {
		printf("HWREG-RPC-FAIL: registry is empty\n");
		return 1;
	}

	walk(svc, root, 0);
	if (failed) {
		printf("HWREG-RPC-FAIL: registry tree walk hit an error\n");
		return 1;
	}

	printf("HWREG-RPC-OK: walked %d registry nodes from root id=%llu\n",
	    walked, (unsigned long long)root);
	return 0;
}
