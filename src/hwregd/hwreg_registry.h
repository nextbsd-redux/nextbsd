/*
 * hwreg_registry.h — hwregd's hardware registry tree (Phase 1).
 *
 * A tree of hw_node records, one per newbus device, modelled on
 * macOS's IORegistry (plan §3.2). Built at startup from a snapshot of
 * the kernel's newbus tree (the hw.bus.* sysctls — the same data
 * libdevinfo exposes; hwregd reads the sysctls directly so it carries
 * no dependency on the FreeBSD-devmatch package, which the image
 * deliberately drops) and kept current by /dev/devctl attach/detach
 * events.
 *
 * iter 1 was the data model + the tree. iter 2a adds the read-side
 * query helpers the Mach-RPC layer (hwreg.defs) calls — and, with the
 * RPC running on hwregd's mach_service_thread while devctl events
 * mutate the tree on the main thread, the registry is now mutex-
 * guarded: every public function below locks internally.
 *
 * Plan: https://pkgdemon.github.io/freebsd-hardware-registry-iokit-plan.html
 */

#ifndef HWREG_REGISTRY_H
#define HWREG_REGISTRY_H

#include <stdint.h>

/* Attachment state of a registry node (plan §3.2). */
enum hw_node_state {
	HW_PROBED = 0,	/* enumerated; driver not (yet) attached */
	HW_ATTACHED,	/* driver attached and live */
	HW_DETACHED,	/* device departed; node lingers for open handles */
	HW_FAILED,	/* driver probe/load failed */
};

/*
 * One node of the hardware registry — a newbus device. iter 1/2a
 * capture the identity and the raw strings the kernel hands us; iter
 * 2b adds the typed xpc property bag the Mach-RPC layer serialises.
 */
struct hw_node {
	uint64_t		id;		/* registry-unique, monotonic; 0 = invalid */
	uint64_t		parent_id;	/* 0 = root of the forest */
	char			name[32];	/* newbus name: "em0", "pci0", "cpu0" */
	char			classname[32];	/* coarse class: "PCIDevice", "USBDevice", ... */
	char			driver[32];	/* attached driver, or "" */
	char			desc[128];	/* human-readable description */
	char			pnpinfo[256];	/* bus plug-and-play string */
	char			location[128];	/* bus location string */
	char			path[256];	/* "/nexus0/.../em0" */
	enum hw_node_state	state;
	uint32_t		refcnt;		/* iter 4 stale-handle detection */
	struct hw_node		*next;		/* registry-internal list link */
};

/*
 * Build the initial registry from a snapshot of the kernel newbus
 * tree. Returns the number of device nodes created, or -1 on failure
 * (errno set). Call once at startup.
 */
int		hwreg_build_snapshot(void);

/* Number of nodes currently in the registry. */
int		hwreg_count(void);

/*
 * Fold a /dev/devctl attach (`+`) or detach (`-`) event into the tree.
 * An attach for an unknown device adds a node (hot-plug); a detach
 * marks the node HW_DETACHED but leaves it in the tree.
 */
void		hwreg_note_attach(const char *name, const char *parent,
		    const char *location);
void		hwreg_note_detach(const char *name);

/* Visit every node in registry order (callback runs under the lock). */
void		hwreg_foreach(void (*fn)(const struct hw_node *n, void *arg),
		    void *arg);

/* --- read-side query API, for the Mach-RPC layer (hwreg.defs) ----- */

/* Registry id of the tree root (first parent-less node); 0 if empty. */
uint64_t	hwreg_root_id(void);

/*
 * Registry ids of `parent_id`'s direct children, written into ids[0..]
 * (at most `max`). Returns the child count (capped at `max`), or -1 if
 * no node has id == parent_id.
 */
int		hwreg_children(uint64_t parent_id, uint64_t *ids, int max);

/*
 * Copy node `id` into *dst. Returns 1 if found (dst filled), 0 if not.
 * dst->next is meaningless in the copy.
 */
int		hwreg_copy(uint64_t id, struct hw_node *dst);

/* Like hwreg_copy(), but keyed on the newbus name. */
int		hwreg_copy_by_name(const char *name, struct hw_node *dst);

/*
 * Adjust node `id`'s reference count. Returns the new count, or -1 if
 * no node has that id. hwreg_node_release() floors the count at 0.
 */
int		hwreg_node_retain(uint64_t id);
int		hwreg_node_release(uint64_t id);

/* Human-readable name for a node state. */
const char     *hw_state_name(enum hw_node_state s);

#endif /* HWREG_REGISTRY_H */
