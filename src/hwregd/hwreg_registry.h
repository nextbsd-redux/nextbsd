/*
 * hwreg_registry.h — hwregd's hardware registry tree (Phase 1 iter 1).
 *
 * A tree of hw_node records, one per newbus device, modelled on
 * macOS's IORegistry (plan §3.2). Built at startup from a snapshot of
 * the kernel's newbus tree (the hw.bus.* sysctls — the same data
 * libdevinfo exposes; hwregd reads the sysctls directly so it carries
 * no dependency on the FreeBSD-devmatch package, which the image
 * deliberately drops) and kept current by /dev/devctl attach/detach
 * events.
 *
 * iter 1 is the data model + the tree itself: no Mach-RPC query API
 * yet (that is iter 2, which also adds the typed xpc property bag and
 * the lock the RPC thread needs). The registry is therefore touched
 * only by hwregd's main/devctl thread in iter 1 and needs no locking.
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
 * One node of the hardware registry — a newbus device. iter 1 captures
 * the identity and the raw strings the kernel hands us; iter 2 adds the
 * typed xpc property bag the Mach-RPC layer serialises.
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
 * Build the initial registry from a snapshot of the kernel newbus tree.
 * Returns the number of device nodes created, or -1 on failure (errno
 * set). Call once at startup.
 */
int		hwreg_build_snapshot(void);

/* Number of nodes currently in the registry. */
int		hwreg_count(void);

/* Look a node up by newbus name / by id; NULL if absent. */
struct hw_node *hwreg_find_by_name(const char *name);
struct hw_node *hwreg_get(uint64_t id);

/*
 * Fold a /dev/devctl attach (`+`) or detach (`-`) event into the tree.
 * An attach for an unknown device adds a node (hot-plug); a detach
 * marks the node HW_DETACHED but leaves it in the tree.
 */
void		hwreg_note_attach(const char *name, const char *parent,
		    const char *location);
void		hwreg_note_detach(const char *name);

/* Visit every node in registry order. */
void		hwreg_foreach(void (*fn)(const struct hw_node *n, void *arg),
		    void *arg);

/* Human-readable name for a node state. */
const char     *hw_state_name(enum hw_node_state s);

#endif /* HWREG_REGISTRY_H */
