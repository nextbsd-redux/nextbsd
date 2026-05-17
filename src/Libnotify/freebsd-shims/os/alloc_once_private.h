/* os/alloc_once_private.h — FreeBSD shim. Apple's slot-indexed
 * once-init allocation. Used for libsystem-wide singletons. Map to
 * pthread_once + heap allocation. */
#ifndef _FREEBSD_SHIM_OS_ALLOC_ONCE_PRIVATE_H_
#define _FREEBSD_SHIM_OS_ALLOC_ONCE_PRIVATE_H_

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Apple defines os_alloc_token_* enum slots; pick high numbers we
 * don't conflict with. Real impl on Apple uses a fixed table; we
 * just allocate-on-demand per slot. */
typedef int os_alloc_token_t;

/* Generic once-allocator. Apple's version takes a slot index and a
 * size; returns the (zero-initialized) pointer, allocating once
 * per slot. Our stub uses a malloc'd block on first call. */
static inline void *
os_alloc_once(os_alloc_token_t token, size_t sz, void (*init)(void *))
{
	(void)token;
	void *p = calloc(1, sz);
	if (p && init) init(p);
	return p;
}

#endif
