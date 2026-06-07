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
 * __io_ioregistry_fd — cached read-only fd to the kernel /dev/ioregistry
 * device (the K1 in-kernel registry, nextbsd#214), opened with
 * O_CLOEXEC on first call (pthread_once-guarded, shared with the
 * hwregd-port lookup). Returns -1 if the device is absent (an
 * old-kernel image predating K1) — every facade entry point then
 * FALLS BACK to the hwregd MIG path. This dual-path is the safety net
 * for the consumer migration (#218) until hwregd is retired (PR7).
 */
int	__io_ioregistry_fd(void);

/*
 * Shared per-op node accessor: prefers the /dev/ioregistry fast path
 * (IOREGIOCNODE) and falls back to hwregd's hwreg_get_node. Both
 * IOKitLib.c (name/path) and IOKitMatching.c (class) call it so the
 * dual-path logic lives in one place. Out-params mirror
 * hwreg_get_node's so call sites change minimally; any pointer may be
 * NULL to skip that field. `state` is the IOREG_STATE_* / hwregd
 * liveness value. Returns a kern_return_t (KERN_SUCCESS on success).
 */
kern_return_t	__io_node(uint64_t node_id, uint64_t *parent_id, int *state,
		    char name[32], char classname[32], char driver[32],
		    char path[256]);

/*
 * Internal allocators. __io_alloc_iterator takes ownership of the
 * id array (it is freed by IOObjectRelease) — on calloc failure the
 * array is freed and IO_OBJECT_NULL returned.
 */
io_object_t	__io_alloc_entry(uint64_t node_id);
io_object_t	__io_alloc_iterator(uint64_t *ids, uint32_t count);

/*
 * The criteria fields hwregd accepts on hwreg_lookup / hwreg_watch
 * — `name`, `class`, `driver` (empty string = wildcard). All three
 * are c_string[32] in hwreg.defs.
 */
#define IO_CRITERIA_FIELD_MAX	32

struct io_criteria {
	char	name[IO_CRITERIA_FIELD_MAX];
	char	klass[IO_CRITERIA_FIELD_MAX];	/* "class" — C++ keyword */
	char	driver[IO_CRITERIA_FIELD_MAX];
};

/*
 * Extract the hwregd-understood criterion strings from an Apple-
 * shape matching CFDictionary. Unknown keys are silently ignored.
 *
 *  IOProviderClass / IOClass	-> out->klass
 *  IONameMatch			-> out->name
 *
 * `matching` may carry other keys (IOPropertyMatch, IOBSDName,
 * regex), all currently ignored.
 */
void	__io_extract_criteria(CFDictionaryRef matching,
	    struct io_criteria *out);

/*
 * Pack a criteria struct into the hwregd wire format (a packed
 * nvlist with the present non-empty fields). Returns KERN_SUCCESS
 * on success. The blob bound (sizeof(hwreg_blob_t) = 2048) bounds
 * the packed payload.
 */
kern_return_t	__io_pack_criteria(const struct io_criteria *c,
		    uint8_t *blob, uint32_t *out_size);

/*
 * Fill a flat kernel `struct ioreg_criteria` from an io_criteria for the
 * /dev/ioregistry IOREGIOC{WATCH,LOOKUP} ioctls (#218). These ioctls now carry
 * the criteria as a fixed by-value struct instead of a packed nvlist: there is
 * no serialization, so the former libxpc-vs-libnv wire-format mismatch that
 * broke the #218 round-trip simply cannot happen. Non-empty string fields and
 * non-zero numeric fields constrain the match (zero-as-wildcard); the hwregd RPC
 * fallback still uses __io_pack_criteria() (libxpc nvlist over MIG, both ends).
 * Declared with a forward struct ref so this header pulls in no kernel ABI.
 */
struct ioreg_criteria;
void	__io_fill_criteria(const struct io_criteria *c,
		    struct ioreg_criteria *out);

#endif /* _IOKIT_INTERNAL_H_ */
