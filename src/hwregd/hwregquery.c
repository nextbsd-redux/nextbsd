/*
 * hwregquery — iter 2a/2b test client for hwregd's Mach-RPC query API.
 *
 * Looks up the org.freebsd.hwregd Mach service, then exercises the MIG
 * hwreg.defs routines: walks the registry tree (hwreg_get_root /
 * hwreg_get_children / hwreg_get_node), unpacks a node's property bag
 * (hwreg_get_properties), runs a criteria lookup (hwreg_lookup), and
 * registers + cancels a device-event watch (hwreg_watch /
 * hwreg_unwatch). Prints HWREG-RPC-OK on success — the CI boot test
 * (run.sh / boot-test.sh) matches that marker. Built against the MIG
 * user stub hwregUser.c.
 */
#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hwreg.h"		/* MIG user-side routine prototypes + types */
#include "nv.h"			/* libxpc nvlist — property bag (de)serialise */

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

	/* hwreg_get_properties — unpack the root node's property bag. */
	{
		hwreg_blob_t props;
		mach_msg_type_number_t propcnt = 0;
		nvlist_t *nv;

		kr = hwreg_get_properties(svc, root, props, &propcnt);
		if (kr != KERN_SUCCESS) {
			printf("HWREG-RPC-FAIL: hwreg_get_properties: 0x%x\n",
			    (unsigned)kr);
			return 1;
		}
		nv = nvlist_unpack(props, propcnt);
		if (nv == NULL) {
			printf("HWREG-RPC-FAIL: property bag did not unpack\n");
			return 1;
		}
		printf("  root props: name=%s class=%s path=%s\n",
		    nvlist_get_string(nv, "name"),
		    nvlist_get_string(nv, "class"),
		    nvlist_get_string(nv, "path"));
		nvlist_destroy(nv);
	}

	/* hwreg_lookup — count the CPU-class nodes via a criteria dict. */
	{
		nvlist_t *crit = nvlist_create_dictionary(0);
		void *packed;
		size_t psz = 0;
		hwreg_blob_t critblob;
		uint64_t ids[MAXKIDS];
		mach_msg_type_number_t nids = 0;

		nvlist_add_string(crit, "class", "CPU");
		packed = nvlist_pack(crit, &psz);
		nvlist_destroy(crit);
		if (packed == NULL || psz > sizeof(critblob)) {
			printf("HWREG-RPC-FAIL: criteria pack failed\n");
			return 1;
		}
		memcpy(critblob, packed, psz);
		free(packed);
		kr = hwreg_lookup(svc, critblob, (mach_msg_type_number_t)psz,
		    ids, &nids);
		if (kr != KERN_SUCCESS) {
			printf("HWREG-RPC-FAIL: hwreg_lookup: 0x%x\n",
			    (unsigned)kr);
			return 1;
		}
		printf("  lookup class=CPU: %u match(es)\n", (unsigned)nids);
	}

	/* hwreg_watch / hwreg_unwatch — register a criteria watch, cancel it. */
	{
		mach_port_t notify = MACH_PORT_NULL;
		nvlist_t *crit = nvlist_create_dictionary(0);
		void *packed;
		size_t psz = 0;
		hwreg_blob_t critblob;
		uint64_t wid = 0;

		kr = mach_port_allocate(mach_task_self(),
		    MACH_PORT_RIGHT_RECEIVE, &notify);
		if (kr != KERN_SUCCESS) {
			printf("HWREG-RPC-FAIL: mach_port_allocate: 0x%x\n",
			    (unsigned)kr);
			return 1;
		}
		nvlist_add_string(crit, "class", "CPU");
		packed = nvlist_pack(crit, &psz);
		nvlist_destroy(crit);
		if (packed == NULL || psz > sizeof(critblob)) {
			printf("HWREG-RPC-FAIL: watch criteria pack failed\n");
			return 1;
		}
		memcpy(critblob, packed, psz);
		free(packed);
		kr = hwreg_watch(svc, critblob, (mach_msg_type_number_t)psz,
		    HWREG_EVT_ARRIVED | HWREG_EVT_DEPARTED, notify, &wid);
		if (kr != KERN_SUCCESS || wid == 0) {
			printf("HWREG-RPC-FAIL: hwreg_watch: 0x%x wid=%llu\n",
			    (unsigned)kr, (unsigned long long)wid);
			return 1;
		}
		printf("  watch class=CPU registered: watcher_id=%llu\n",
		    (unsigned long long)wid);
		kr = hwreg_unwatch(svc, wid);
		if (kr != KERN_SUCCESS) {
			printf("HWREG-RPC-FAIL: hwreg_unwatch: 0x%x\n",
			    (unsigned)kr);
			return 1;
		}
	}

	printf("HWREG-RPC-OK: walked %d nodes; props, lookup, watch OK "
	    "(root id=%llu)\n", walked, (unsigned long long)root);
	return 0;
}
