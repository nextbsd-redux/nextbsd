/* os/lock_private.h — FreeBSD shim. Apple's os_unfair_lock is a
 * spinlock-style userland lock. Map to pthread_mutex for now —
 * functionally equivalent for correctness, slower for uncontended
 * cases. Replace with FreeBSD umtx/_umtx_op-based equivalent if it
 * becomes a hotspot. */
#ifndef _FREEBSD_SHIM_OS_LOCK_PRIVATE_H_
#define _FREEBSD_SHIM_OS_LOCK_PRIVATE_H_

#include <pthread.h>
#include <stdint.h>

typedef pthread_mutex_t os_unfair_lock;
typedef os_unfair_lock os_unfair_lock_s;
typedef pthread_mutex_t * os_unfair_lock_t;

#define OS_UNFAIR_LOCK_INIT		PTHREAD_MUTEX_INITIALIZER

#define os_unfair_lock_lock(l)		(void)pthread_mutex_lock(l)
#define os_unfair_lock_unlock(l)	(void)pthread_mutex_unlock(l)
#define os_unfair_lock_trylock(l)	(pthread_mutex_trylock(l) == 0)
#define os_unfair_lock_assert_owner(l)	(void)0
#define os_unfair_lock_assert_not_owner(l) (void)0

/* Apple's extended lock-with-options. We ignore the options flag
 * — the priority-inheritance / data-synchronization variants are
 * macOS scheduler hints with no FreeBSD equivalent. */
typedef uint32_t os_unfair_lock_options_t;

#define OS_UNFAIR_LOCK_NONE				0x00000000
#define OS_UNFAIR_LOCK_DATA_SYNCHRONIZATION		0x00010000
#define OS_UNFAIR_LOCK_ADAPTIVE_SPIN			0x00040000

#define os_unfair_lock_lock_with_options(l, opts) \
	(void)pthread_mutex_lock(l)
#define os_unfair_lock_trylock_with_options(l, opts) \
	(pthread_mutex_trylock(l) == 0)

#endif
