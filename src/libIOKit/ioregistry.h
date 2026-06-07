/*
 * VENDORED COPY — keep in sync with the kernel canonical.
 *
 * Canonical: nextbsd-kernel src-overlay/sys/sys/ioregistry.h (installed as
 * <sys/ioregistry.h> in the kernel sysroot). Userland in this repo cannot
 * include the kernel src-overlay directly, so libIOKit carries a verbatim
 * copy of the K1 (#214) ABI here — the same convention kextd uses for its
 * vendored sys/iocatalogue.h. If the canonical struct/ioctl definitions ever
 * change, update this file to match; the ABI is frozen once shipped, so this
 * mirror should rarely move.
 *
 * --- canonical contents below, verbatim ---
 *
 * NextBSD in-kernel IORegistry (K1, nextbsd#214).
 *
 * A read-only, on-demand in-kernel view of the live newbus device_t tree,
 * presented as IOKit-style nodes + property bags and queryable from userland
 * via ioctl on /dev/ioregistry. This is the kernel-served replacement for
 * hwregd's tree: hwregd is merely a userland cache of hw.bus sysctls plus
 * /dev/devctl events, whereas this walks the real device_t tree directly.
 *
 * PR1 (#214) is kernel-only and read-only: it builds no shadow tree, it walks
 * the live newbus topology on demand. A stable uint64 registry id is minted per
 * device_t (and lingers, marked detached, after the device goes away) so a
 * userland iterator never dangles. The libIOKit migration (later PR) consumes
 * this ABI; keep these structs frozen once shipped.
 *
 * See pkgdemon.github.io/nextbsd-inkernel-iokit-feasibility.html.
 */
#ifndef _SYS_IOREGISTRY_H_
#define _SYS_IOREGISTRY_H_

#include <sys/types.h>
#include <sys/ioccom.h>

#define	IOREG_NAME_MAX		32	/* device name / class / driver field */
#define	IOREG_PATH_MAX		256	/* full newbus path, incl. NUL */
#define	IOREG_CRIT_MAX		65536	/* max packed criteria nvlist (bytes) */

/*
 * Node lifecycle state (ioreg_node.state). Distinct from the newbus
 * device_state_t: this is the registry id's own liveness, so a userland
 * iterator can tell a live node from one that detached but has not yet aged
 * out (linger-until-unreferenced, mirroring hwregd).
 */
#define	IOREG_STATE_LIVE	0	/* device_t currently present */
#define	IOREG_STATE_DETACHED	1	/* device gone; id lingering */

/*
 * One registry node. Fixed-size and self-contained so the ABI is identical for
 * 32- and 64-bit userland. `id` is the stable registry id (never reused while
 * lingering); `parent_id` is 0 for the root. The pci_* fields are 0 unless the
 * node is a PCI nub (its parent bus is "pci"); pci_class holds the 8-bit PCI
 * base class in its low bits.
 */
struct ioreg_node {
	uint64_t	id;			/* stable registry id; 0 = invalid */
	uint64_t	parent_id;		/* parent's id, 0 if root */
	int32_t		state;			/* IOREG_STATE_* */
	int32_t		devstate;		/* newbus device_state_t (DS_*) */
	char		name[IOREG_NAME_MAX];	/* device_get_name() */
	char		classname[IOREG_NAME_MAX]; /* devclass name */
	char		driver[IOREG_NAME_MAX];	/* bound driver name, "" if none */
	char		path[IOREG_PATH_MAX];	/* nameunit path from root */
	uint32_t	pci_vendor;		/* PCI vendor id, 0 if n/a */
	uint32_t	pci_device;		/* PCI device id, 0 if n/a */
	uint32_t	pci_subvendor;		/* PCI subsystem vendor, 0 if n/a */
	uint32_t	pci_class;		/* PCI base class (low 8 bits) */
};

/*
 * Enumerate a node's immediate children. Userland sets `id` (the parent) and
 * `max` (capacity of the `children` array, a user ptr to uint64_t[max]); the
 * kernel writes up to `max` child ids and sets `count` to the number of
 * children that exist. If count > max the array was truncated — userland should
 * grow the buffer and retry. `children` is a uint64_t for 32/64 ABI parity.
 */
struct ioreg_children {
	uint64_t	id;		/* in: parent node id */
	uint32_t	max;		/* in: capacity of children[] */
	uint32_t	count;		/* out: number of children that exist */
	uint64_t	children;	/* in: user ptr to uint64_t[max] */
};

/*
 * Fetch a node's property bag as a packed nvlist(9). Userland sets `id` and
 * `len` (capacity of `buf`, a user ptr); the kernel packs the bag, writes it
 * (up to `len` bytes) to buf, and sets `len` to the full packed size. If the
 * returned len > the input len the buffer was too small — userland grows it and
 * retries. A NULL buf (len ignored) just sizes the bag. The bag carries the
 * same keys as ioreg_node plus any device_has_property()-visible scalars.
 */
struct ioreg_props {
	uint64_t	id;		/* in: node id */
	uint32_t	len;		/* in: buf capacity; out: full packed size */
	uint32_t	_pad;
	uint64_t	buf;		/* in: user ptr to packed nvlist bytes */
};

/*
 * Look up nodes matching a packed-nvlist criteria bag (e.g. {"name":"pci"} or
 * {"pci_vendor":0x8086}). Userland sets `buf_criteria`/`crit_len` (a packed
 * nvlist of scalar keys, AND-matched against each node) and `max` (capacity of
 * the `matches` array, user ptr to uint64_t[max]); the kernel writes up to
 * `max` matching ids and sets `count` to the total number of matches. count >
 * max means truncation. crit_len == 0 matches every live node.
 */
struct ioreg_lookup {
	uint64_t	buf_criteria;	/* in: user ptr to packed nvlist criteria */
	uint32_t	crit_len;	/* in: length of criteria bag, 0 = match all */
	uint32_t	max;		/* in: capacity of matches[] */
	uint32_t	count;		/* out: total number of matches */
	uint32_t	_pad;
	uint64_t	matches;	/* in: user ptr to uint64_t[max] */
};

#define	IOREGIOCROOT	_IOR('R', 1, uint64_t)		   /* get root node id */
#define	IOREGIOCCHILDREN _IOWR('R', 2, struct ioreg_children) /* enum children */
#define	IOREGIOCNODE	_IOWR('R', 3, struct ioreg_node)   /* node by id (in id) */
#define	IOREGIOCPROPS	_IOWR('R', 4, struct ioreg_props)  /* node property bag */
#define	IOREGIOCLOOKUP	_IOWR('R', 5, struct ioreg_lookup) /* match by criteria */

#endif /* _SYS_IOREGISTRY_H_ */
