/* configuration_profile.h — FreeBSD shim. Apple's
 * configuration_profile_copy_property_list() reads MDM-installed
 * profile overrides. FreeBSD has no MDM; return NULL (no profile
 * overrides). asl_common.c + asl_action.c use this to merge
 * /Library/Managed Preferences/-style overrides into asl.conf;
 * stub means "no overrides, use the on-disk asl.conf as-is."
 */
#ifndef _FREEBSD_SHIM_CONFIGURATION_PROFILE_H_
#define _FREEBSD_SHIM_CONFIGURATION_PROFILE_H_

#include <stddef.h>

/* Forward-declare xpc_object_t opaque rather than including xpc.h
 * — keeps dependency chain shallow. */
typedef void * xpc_object_t;

static inline xpc_object_t
configuration_profile_copy_property_list(const char *path)
{
	(void)path;
	return NULL;
}

#endif
