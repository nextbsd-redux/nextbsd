/* libkern/OSAtomic.h — FreeBSD shim. Apple's pre-C11 atomic API.
 * libsystem_asl uses OSAtomicIncrement32() etc. for refcounting.
 * Map to C11 atomics. */
#ifndef _FREEBSD_SHIM_LIBKERN_OSATOMIC_H_
#define _FREEBSD_SHIM_LIBKERN_OSATOMIC_H_

#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>

/* Apple's OSAtomic* functions return the NEW value (post-op). C11
 * atomic_fetch_* return the OLD value, so we add/subtract after.
 * Both barrier and non-barrier variants map to memory_order_seq_cst
 * (safest); the non-barrier variants would be memory_order_relaxed
 * but ASL doesn't differentiate by perf, just correctness. */

static inline int32_t
OSAtomicAdd32(int32_t amt, volatile int32_t *p)
{
	return atomic_fetch_add((_Atomic int32_t *)p, amt) + amt;
}

static inline int32_t
OSAtomicAdd32Barrier(int32_t amt, volatile int32_t *p)
{
	return OSAtomicAdd32(amt, p);
}

static inline int32_t
OSAtomicIncrement32(volatile int32_t *p)
{
	return OSAtomicAdd32(1, p);
}

static inline int32_t
OSAtomicIncrement32Barrier(volatile int32_t *p)
{
	return OSAtomicAdd32(1, p);
}

static inline int32_t
OSAtomicDecrement32(volatile int32_t *p)
{
	return OSAtomicAdd32(-1, p);
}

static inline int32_t
OSAtomicDecrement32Barrier(volatile int32_t *p)
{
	return OSAtomicAdd32(-1, p);
}

static inline int64_t
OSAtomicAdd64(int64_t amt, volatile int64_t *p)
{
	return atomic_fetch_add((_Atomic int64_t *)p, amt) + amt;
}

static inline int64_t
OSAtomicIncrement64(volatile int64_t *p)
{
	return OSAtomicAdd64(1, p);
}

static inline int64_t
OSAtomicDecrement64(volatile int64_t *p)
{
	return OSAtomicAdd64(-1, p);
}

static inline bool
OSAtomicCompareAndSwap32(int32_t old_val, int32_t new_val, volatile int32_t *p)
{
	return atomic_compare_exchange_strong((_Atomic int32_t *)p, &old_val, new_val);
}

static inline bool
OSAtomicCompareAndSwap32Barrier(int32_t old_val, int32_t new_val, volatile int32_t *p)
{
	return OSAtomicCompareAndSwap32(old_val, new_val, p);
}

static inline bool
OSAtomicCompareAndSwap64(int64_t old_val, int64_t new_val, volatile int64_t *p)
{
	return atomic_compare_exchange_strong((_Atomic int64_t *)p, &old_val, new_val);
}

static inline bool
OSAtomicCompareAndSwapPtr(void *old_val, void *new_val, void * volatile *p)
{
	return atomic_compare_exchange_strong((_Atomic(void *) *)p,
	    (void **)&old_val, new_val);
}

/* Apple's "Long" CAS is platform-native long. amd64 / arm64 = 64-bit
 * (LP64); fall through to the 64-bit primitive. */
static inline bool
OSAtomicCompareAndSwapLong(long old_val, long new_val, volatile long *p)
{
	return atomic_compare_exchange_strong((_Atomic long *)p,
	    &old_val, new_val);
}

static inline bool
OSAtomicCompareAndSwapLongBarrier(long old_val, long new_val, volatile long *p)
{
	return OSAtomicCompareAndSwapLong(old_val, new_val, p);
}

#endif
