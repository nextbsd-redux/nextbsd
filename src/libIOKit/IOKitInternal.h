/*
 * IOKitInternal.h — private to libIOKit.
 *
 * Defines the client-side handle struct that backs every io_object_t
 * the facade hands out, plus the shared lazy-init helper for the
 * hwregd Mach service port. iter 1 — read-only walk; later iters
 * extend the struct as new handle flavors appear (matching
 * iterators, notification iterators).
 */
#ifndef _IOKIT_INTERNAL_H_
#define _IOKIT_INTERNAL_H_

#include <IOKit/IOKitLib.h>

#include <stdatomic.h>
#include <stdint.h>
#include <mach/mach.h>

/*
 * Handle flavors. iter 1 only has two — a registry entry (a single
 * hwregd node id) and an iterator (a captured uint64_t id array).
 * Each iter that adds a new flavor adds a constant here and a
 * branch in IOObjectRelease.
 */
#define IOOBJ_KIND_ENTRY	1
#define IOOBJ_KIND_ITERATOR	2

struct __IOObject {
	uint32_t	kind;		/* IOOBJ_KIND_* */
	atomic_int	refcnt;		/* IOObjectRetain/Release */
	/* entry: */
	uint64_t	node_id;
	/* iterator: */
	uint64_t	*ids;		/* malloced; freed in release */
	uint32_t	count;
	uint32_t	cursor;
};

/*
 * __io_hwregd_port — cached send right to org.freebsd.hwregd, looked
 * up via bootstrap_look_up on first call (pthread_once-guarded).
 * Returns MACH_PORT_NULL if the lookup fails; callers report the
 * facade routine as kIOReturnNoDevice / IO_OBJECT_NULL.
 */
mach_port_t	__io_hwregd_port(void);

/*
 * Internal allocators. __io_alloc_iterator takes ownership of the
 * id array (it is freed by IOObjectRelease) — on calloc failure the
 * array is freed and IO_OBJECT_NULL returned.
 */
io_object_t	__io_alloc_entry(uint64_t node_id);
io_object_t	__io_alloc_iterator(uint64_t *ids, uint32_t count);

#endif /* _IOKIT_INTERNAL_H_ */
