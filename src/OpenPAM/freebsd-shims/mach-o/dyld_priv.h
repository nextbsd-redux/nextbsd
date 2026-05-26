/*
 * <mach-o/dyld_priv.h> — FreeBSD shim for Apple dyld's private
 * interface, used by OpenPAM's openpam_dynamic.c
 * (_dyld_shared_cache_contains_path()) to fast-path module loads
 * out of the macOS dyld shared cache.
 *
 * FreeBSD uses ELF + rtld(1); there is no dyld and no shared
 * cache. The fast-path check trivially returns false on FreeBSD,
 * which causes openpam_dlopen() to fall through to its
 * conventional faccessat() + dlopen() slow path — exactly what we
 * want on a system without a dyld shared cache.
 *
 * Vendored under src/OpenPAM/freebsd-shims/ — added to CFLAGS via
 * the Makefile's -I, NOT installed to the rootfs.
 */
#ifndef _FREEBSD_SHIMS_MACH_O_DYLD_PRIV_H_
#define _FREEBSD_SHIMS_MACH_O_DYLD_PRIV_H_

#include <stdbool.h>

/* _dyld_shared_cache_contains_path — no-op stub.
 * On macOS, returns true when `path` is mapped from the dyld
 * shared cache (a pre-linked blob of system libraries). FreeBSD
 * has no such cache, so the answer is always false. The caller
 * then takes the conventional faccessat() + dlopen() path. */
static __inline bool
_dyld_shared_cache_contains_path(const char *path)
{
	(void)path;
	return (false);
}

#endif /* _FREEBSD_SHIMS_MACH_O_DYLD_PRIV_H_ */
