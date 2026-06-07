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

/*
 * Flat, fixed-size device match criteria (nextbsd#218). Replaces the former
 * packed-nvlist criteria bag that IOREGIOCLOOKUP / IOREGIOCWATCH carried: the
 * userland packer (libxpc's nvlist) and the kernel reader (base libnv) use
 * incompatible wire formats, so a packed bag round-tripped through the ioctl
 * never unpacked. A flat struct has NO serialization, so there is nothing to
 * mismatch — both sides simply read the same fixed fields.
 *
 * Match semantics: a field that is the empty string (for the char[] fields) or
 * zero (for the numeric fields) is a WILDCARD and does not constrain the match;
 * any non-empty / non-zero field MUST equal the candidate node's corresponding
 * value (strcmp for strings, == for numerics). All set fields are AND-ed; an
 * all-zero criteria therefore matches every node. Self-contained / fixed-size,
 * so the ABI is identical for 32- and 64-bit userland.
 */
struct ioreg_criteria {
	char		name[IOREG_NAME_MAX];	/* device_get_name(); "" = any */
	char		classname[IOREG_NAME_MAX]; /* devclass name; "" = any */
	char		driver[IOREG_NAME_MAX];	/* bound driver name; "" = any */
	uint32_t	pci_vendor;		/* PCI vendor id; 0 = any */
	uint32_t	pci_device;		/* PCI device id; 0 = any */
	uint32_t	pci_subvendor;		/* PCI subsystem vendor; 0 = any */
	uint32_t	pci_class;		/* PCI base class (low 8 bits); 0 = any */
	uint32_t	match_flags;		/* reserved; 0 (zero-as-wildcard) */
};

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
 * Look up nodes matching a flat criteria struct (e.g. {.name = "pci"} or
 * {.pci_vendor = 0x8086}). Userland fills `criteria` (zero-as-wildcard,
 * AND-matched against each node; see struct ioreg_criteria) and `max` (capacity
 * of the `matches` array, user ptr to uint64_t[max]); the kernel writes up to
 * `max` matching ids and sets `count` to the total number of matches. count >
 * max means truncation. An all-zero criteria matches every live node.
 */
struct ioreg_lookup {
	struct ioreg_criteria criteria;	/* in: flat match criteria */
	uint32_t	max;		/* in: capacity of matches[] */
	uint32_t	count;		/* out: total number of matches */
	uint64_t	matches;	/* in: user ptr to uint64_t[max] */
};

/*
 * Device-event notification (K-PR2, nextbsd#225). A userland client (e.g.
 * DiskArbitration, the IOKitNotify migration) creates a Mach receive right,
 * then registers it here so the kernel pushes a message to that port whenever a
 * device whose newbus identity matches `criteria` arrives or departs. This is
 * the kernel-served replacement for hwregd watching /dev/devctl: the kernel
 * emits the events itself from the device_attach / device_detach eventhandlers.
 *
 * The faithful IOKit shape: the client owns the receive right; the kernel holds
 * a copied send right and sends the on-the-wire event message (struct
 * ioreg_event_msg, defined in <sys/mach/iokit_notify.h> with the wire msgid) on
 * each matching event whose kind is in `event_mask`. The send right is dropped
 * (and the watch retired) when the client's port goes dead.
 */

/* event_mask bits and ioreg_event_msg.kind values (faithful to IOKit's notion
 * of matched/published vs. terminated). A watch with event_mask == 0 receives
 * nothing; the common case is IOREG_EVENT_ARRIVE | IOREG_EVENT_DEPART. */
#define	IOREG_EVENT_ARRIVE	0x00000001	/* device attached (published) */
#define	IOREG_EVENT_DEPART	0x00000002	/* device detached (terminated) */
#define	IOREG_EVENT_MATCHED	0x00000004	/* device bound a driver */

/*
 * IOREGIOCWATCH argument. `criteria` is the same flat AND-match struct used by
 * IOREGIOCLOOKUP (fields name/class/driver/pci_*; zero-as-wildcard, an all-zero
 * criteria means "match every device"). `event_mask` is the OR of the
 * IOREG_EVENT_* kinds to deliver. `notify_port` is the *name*, in the calling
 * task's IPC space, of the receive right whose send right the kernel copies and
 * keeps; the client keeps the receive right and reads ioreg_event_msg from it.
 * Self-contained / fixed-size for 32- and 64-bit ABI parity.
 */
struct ioreg_watch_reg {
	struct ioreg_criteria criteria;	/* in: flat match criteria */
	uint32_t	event_mask;	/* in: OR of IOREG_EVENT_* to deliver */
	uint32_t	notify_port;	/* in: mach_port_name_t (recv right name) */
};

/*
 * IOREGIOCTESTEVENT (PR4/C1.2, nextbsd#225/#218): deterministic test injection
 * of a synthetic device event. The notify channel (IOREGIOCWATCH) normally
 * pushes only on real device_attach / device_detach, which CI cannot easily
 * synthesize without a physical device. This ioctl feeds a caller-supplied
 * fake event {kind, id, name, classname, pci_vendor, pci_device} through the
 * SAME watch match + ioreg_event_msg emission path a real device_attach takes,
 * so any registered watch whose criteria match receives a genuine
 * ioreg_event_msg. It exercises match + Mach send end-to-end with no device.
 *
 * `kind` is exactly ONE IOREG_EVENT_* bit (the event being injected). The
 * string fields are NUL-terminated (the kernel re-bounds them). This mirrors
 * the catalogue's IOCATIOCTESTSEND de-risk ioctl: a test affordance, inert in
 * the build until a userland test fires it. Returns 0 on success, EINVAL for a
 * malformed event, ENOSYS on a kernel built without COMPAT_MACH (no Mach
 * channel). Self-contained / fixed-size for 32- and 64-bit ABI parity.
 */
struct ioreg_test_event {
	uint32_t	kind;			/* in: exactly one IOREG_EVENT_* */
	uint32_t	pci_vendor;		/* in: synthetic PCI vendor, 0 if n/a */
	uint32_t	pci_device;		/* in: synthetic PCI device, 0 if n/a */
	uint32_t	_pad;
	uint64_t	id;			/* in: synthetic registry node id */
	char		name[IOREG_NAME_MAX];	/* in: device name */
	char		classname[IOREG_NAME_MAX]; /* in: devclass name */
};

#define	IOREGIOCROOT	_IOR('R', 1, uint64_t)		   /* get root node id */
#define	IOREGIOCCHILDREN _IOWR('R', 2, struct ioreg_children) /* enum children */
#define	IOREGIOCNODE	_IOWR('R', 3, struct ioreg_node)   /* node by id (in id) */
#define	IOREGIOCPROPS	_IOWR('R', 4, struct ioreg_props)  /* node property bag */
#define	IOREGIOCLOOKUP	_IOWR('R', 5, struct ioreg_lookup) /* match by criteria */
#define	IOREGIOCWATCH	_IOW('R', 6, struct ioreg_watch_reg) /* register notify */
#define	IOREGIOCTESTEVENT _IOW('R', 7, struct ioreg_test_event) /* inject event */

#endif /* _SYS_IOREGISTRY_H_ */
