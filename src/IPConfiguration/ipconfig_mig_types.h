/*
 * ipconfig_mig_types.h — C typedefs for the named MIG types in
 * ipconfig.defs.
 *
 * MIG records the wire layout of a `type` declaration but does not
 * emit a C typedef for it: the generated stubs use the type name
 * as-is and expect an imported header to define it. ipconfig.defs
 * imports this header so both the generated server (ipconfigServer
 * .{c,h}) and user (ipconfigUser.c / ipconfig.h) sides agree.
 *
 * Slimmed from Apple's bootp/bootplib/ipconfig_types.h: only the
 * names ipconfig.defs references. Apple's header pulls cfutil.h +
 * util.h + a wall of SystemConfiguration #defines we don't need
 * yet — vendor those when a routine that actually uses them lands.
 */
#ifndef _IPCONFIG_MIG_TYPES_H
#define _IPCONFIG_MIG_TYPES_H

#include <stdint.h>
#include <net/if.h>		/* IF_NAMESIZE */

/* Matches Apple's ipconfig_types.h:
 *   typedef char InterfaceName[IF_NAMESIZE];
 * MIG renders if_name = array[16] of char as `char if_name[16]` —
 * with `ctype: InterfaceName`, the generated stubs use the typedef. */
typedef char	InterfaceName[IF_NAMESIZE];

typedef uint32_t	ip_address_t;

/*
 * Apple's enum is a 21-value status code (success / permission-denied /
 * interface-does-not-exist / ...). iter 5a uses three of them;
 * preserve the numeric values + names so a later iter can append
 * without breaking wire compat. The enum is copied from
 * bootp/bootplib/ipconfig_types.h.
 */
typedef enum {
	ipconfig_status_success_e			= 0,
	ipconfig_status_permission_denied_e		= 1,
	ipconfig_status_interface_does_not_exist_e	= 2,
	ipconfig_status_invalid_parameter_e		= 3,
	ipconfig_status_invalid_operation_e		= 4,
	ipconfig_status_allocation_failed_e		= 5,
	ipconfig_status_internal_error_e		= 6,
	ipconfig_status_operation_not_supported_e	= 7,
	ipconfig_status_address_in_use_e		= 8,
	ipconfig_status_no_server_e			= 9,
	ipconfig_status_server_not_responding_e		= 10,
	ipconfig_status_lease_terminated_e		= 11,
	ipconfig_status_media_inactive_e		= 12,
	ipconfig_status_server_error_e			= 13,
	ipconfig_status_no_such_service_e		= 14,
	ipconfig_status_duplicate_service_e		= 15,
	ipconfig_status_address_timed_out_e		= 16,
	ipconfig_status_not_found_e			= 17,
	ipconfig_status_resource_unavailable_e		= 18,
	ipconfig_status_network_changed_e		= 19,
	ipconfig_status_lease_expired_e			= 20,
	ipconfig_status_dhcp_waiting_e			= 21
} ipconfig_status_t;

#endif /* _IPCONFIG_MIG_TYPES_H */
