/* os/atomic_private.h — FreeBSD shim. Apple's os_atomic_* wraps
 * C11 atomics + memory ordering. Map to <stdatomic.h>. */
#ifndef _FREEBSD_SHIM_OS_ATOMIC_PRIVATE_H_
#define _FREEBSD_SHIM_OS_ATOMIC_PRIVATE_H_

#include <stdatomic.h>

/* Apple memory orderings — map to stdatomic equivalents. */
#define os_atomic_load(p, m)		atomic_load_explicit((p), memory_order_##m)
#define os_atomic_store(p, v, m)	atomic_store_explicit((p), (v), memory_order_##m)
#define os_atomic_add(p, v, m)		atomic_fetch_add_explicit((p), (v), memory_order_##m)
#define os_atomic_sub(p, v, m)		atomic_fetch_sub_explicit((p), (v), memory_order_##m)
#define os_atomic_inc(p, m)		atomic_fetch_add_explicit((p), 1, memory_order_##m)
#define os_atomic_dec(p, m)		atomic_fetch_sub_explicit((p), 1, memory_order_##m)
#define os_atomic_or(p, v, m)		atomic_fetch_or_explicit((p), (v), memory_order_##m)
#define os_atomic_and(p, v, m)		atomic_fetch_and_explicit((p), (v), memory_order_##m)
#define os_atomic_xor(p, v, m)		atomic_fetch_xor_explicit((p), (v), memory_order_##m)
#define os_atomic_cmpxchg(p, e, v, m)	({ __typeof(e) _e = (e); \
                                           atomic_compare_exchange_strong_explicit( \
                                               (p), &_e, (v), \
                                               memory_order_##m, memory_order_##m); })
#define os_atomic_cmpxchgv(p, e, v, gp, m) ({ __typeof(e) _e = (e); \
                                           bool _ok = atomic_compare_exchange_strong_explicit( \
                                               (p), &_e, (v), \
                                               memory_order_##m, memory_order_##m); \
                                           *(gp) = _e; _ok; })
#define os_atomic_rmw_loop(p, ov, nv, m, ...) ({ \
    __typeof(*p) ov; __typeof(*p) nv; \
    do { ov = atomic_load_explicit((p), memory_order_##m); __VA_ARGS__ } \
    while (!atomic_compare_exchange_weak_explicit( \
        (p), &ov, nv, memory_order_##m, memory_order_##m)); \
    1; })
#define os_atomic_thread_fence(m)	atomic_thread_fence(memory_order_##m)

/* Hint constants */
#define os_atomic_init(p, v)		atomic_init((p), (v))

#endif
