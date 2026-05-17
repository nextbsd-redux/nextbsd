/* mach-o/dyld_priv.h — FreeBSD shim. Apple's dyld private API.
 * notify_client.c uses only _dyld_is_memory_immutable() to check
 * whether a string pointer lives in a read-only segment (so the
 * notification system can store it without copying). On FreeBSD
 * there's no equivalent dyld query — return false conservatively
 * so libnotify always copies. */
#ifndef _FREEBSD_SHIM_MACH_O_DYLD_PRIV_H_
#define _FREEBSD_SHIM_MACH_O_DYLD_PRIV_H_

#include <stdbool.h>
#include <stddef.h>

static inline bool
_dyld_is_memory_immutable(const void *addr, size_t size)
{
	(void)addr;
	(void)size;
	return false;
}

#endif
