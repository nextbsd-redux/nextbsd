/* os/collections.h — FreeBSD shim. Apple's intrusive list / set /
 * map types used by libnotify's notify_state_t. Stub to opaque
 * void* pointers — the real Apple impl is a thread-safe hash set
 * with C++-style iterators; we provide enough to compile, with the
 * understanding that real-runtime correctness requires either
 * (a) a from-scratch hash table impl in this shim, or (b) gating
 * libnotify's collection-using paths off until a real port lands.
 *
 * For J1 (compile-only), all collection ops are no-ops. J2's
 * notifyd port — which is where these actually MATTER — will need
 * to add real implementations OR replace the set/map uses with
 * BSD <sys/queue.h> primitives.
 */
#ifndef _FREEBSD_SHIM_OS_COLLECTIONS_H_
#define _FREEBSD_SHIM_OS_COLLECTIONS_H_

#include <sys/queue.h>
#include <stddef.h>
#include <stdint.h>

/* Opaque set / map types. Apple's actual definitions are in
 * <os/collections.h> on macOS and back hash-table impls. */
typedef void * os_set_str_ptr_t;
typedef void * os_set_32_ptr_t;
typedef void * os_set_64_ptr_t;
typedef void * os_set_ptr_t;

typedef void * os_map_str_t;
typedef void * os_map_32_t;
typedef void * os_map_64_t;
typedef void * os_map_ptr_t;

/* Stub ops — all no-ops. Real impl in J2 if/when needed. */
#define os_map_init(m, ops)			do { *(m) = NULL; } while (0)
#define os_map_destroy(m)			(void)0
#define os_map_get(m, k)			NULL
#define os_map_insert(m, k, v)			(void)0
#define os_map_delete(m, k)			(void)0
#define os_map_foreach(m, k, v)			if (0)
#define os_map_count(m)				0

#define os_set_init(s, ops)			do { *(s) = NULL; } while (0)
#define os_set_destroy(s)			(void)0
#define os_set_insert(s, k)			(void)0
#define os_set_remove(s, k)			(void)0
#define os_set_contains(s, k)			false
#define os_set_count(s)				0

#endif
