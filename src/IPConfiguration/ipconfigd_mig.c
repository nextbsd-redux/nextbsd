/*
 * ipconfigd_mig.c — server-side bodies for ipconfig.defs.
 *
 * The MIG-generated demux _ipconfig_server() (in ipconfigServer.c)
 * calls these per routine. Both iter-5a routines are read-only and
 * pull live data from bound_state.{c,h}.
 *
 * Apple's per-routine bodies live inline in IPConfiguration.bproj/
 * ipconfigd.c (10k+ LOC of plugin shell); we keep ours in a small
 * dedicated TU so the daemon main and the lease-loop stay clean.
 */

#include "bound_state.h"
#include "ipconfig_mig_types.h"

#include <mach/mach.h>
#include "ipconfigServer.h"		/* MIG-emitted prototypes */

#include <string.h>

/*
 * iter 5a: scalar out, no input. Returns 0 or 1 depending on
 * whether ipconfigd has any interface BOUND.
 */
kern_return_t
_ipconfig_if_count(mach_port_t server, int *count)
{
	(void)server;
	*count = bound_state_count();
	return (KERN_SUCCESS);
}

/*
 * iter 5a: input if_name + scalar out + status. Returns the BOUND
 * IPv4 address as a network-order packed uint32 (matches Apple's
 * ip_address_t wire encoding). Unknown / unbound interfaces return
 * ipconfig_status_no_server_e.
 *
 * MIG passes `name` as an array[16] of char — may not be NUL
 * terminated if the client filled all 16 bytes. Force a NUL at the
 * last slot before strcmp.
 */
kern_return_t
_ipconfig_if_addr(mach_port_t server, InterfaceName name,
    ip_address_t *addr, ipconfig_status_t *status)
{
	char ifname[IF_NAMESIZE];
	uint32_t a = 0;

	(void)server;
	(void)memset(ifname, 0, sizeof(ifname));
	(void)memcpy(ifname, name, sizeof(ifname) - 1);

	if (bound_state_get_addr(ifname, &a)) {
		*addr = a;
		*status = ipconfig_status_success_e;
	} else {
		*addr = 0;
		*status = ipconfig_status_no_server_e;
	}
	return (KERN_SUCCESS);
}
