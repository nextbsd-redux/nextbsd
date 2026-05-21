/*
 * hwreg_registry.c — hwregd hardware registry tree (Phase 1 iter 1).
 *
 * Builds and maintains the hw_node tree described in hwreg_registry.h.
 * The boot snapshot comes from the kernel newbus tree via the hw.bus.*
 * sysctls (hw.bus.info for the ABI version, hw.bus.devices.<N> for one
 * struct u_device per device) — the same data libdevinfo reads, but
 * read directly so hwregd needs no FreeBSD-devmatch dependency. After
 * the snapshot, /dev/devctl attach/detach events keep the tree current.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/sysctl.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hwreg_registry.h"

/*
 * The registry is a singly-linked list kept in enumeration order
 * (head .. tail). iter 1 is single-threaded — only hwregd's devctl
 * thread touches it — so there is no lock; iter 2's Mach-RPC reader
 * thread adds one.
 */
static struct hw_node *registry_head;
static struct hw_node *registry_tail;
static uint64_t		next_id = 1;	/* 0 is reserved for "no node" */
static int		node_count;

const char *
hw_state_name(enum hw_node_state s)
{
	switch (s) {
	case HW_PROBED:		return "PROBED";
	case HW_ATTACHED:	return "ATTACHED";
	case HW_DETACHED:	return "DETACHED";
	case HW_FAILED:		return "FAILED";
	}
	return "?";
}

int
hwreg_count(void)
{
	return node_count;
}

struct hw_node *
hwreg_find_by_name(const char *name)
{
	struct hw_node *n;

	if (name == NULL || name[0] == '\0')
		return NULL;
	for (n = registry_head; n != NULL; n = n->next)
		if (strcmp(n->name, name) == 0)
			return n;
	return NULL;
}

struct hw_node *
hwreg_get(uint64_t id)
{
	struct hw_node *n;

	if (id == 0)
		return NULL;
	for (n = registry_head; n != NULL; n = n->next)
		if (n->id == id)
			return n;
	return NULL;
}

void
hwreg_foreach(void (*fn)(const struct hw_node *n, void *arg), void *arg)
{
	struct hw_node *n;

	for (n = registry_head; n != NULL; n = n->next)
		fn(n, arg);
}

/* Append a freshly-allocated node for `name`; returns NULL on OOM. */
static struct hw_node *
node_new(const char *name, uint64_t parent_id)
{
	struct hw_node *n = calloc(1, sizeof(*n));

	if (n == NULL)
		return NULL;
	n->id = next_id++;
	n->parent_id = parent_id;
	strlcpy(n->name, name, sizeof(n->name));
	n->state = HW_PROBED;
	if (registry_tail == NULL)
		registry_head = n;
	else
		registry_tail->next = n;
	registry_tail = n;
	node_count++;
	return n;
}

/*
 * Coarse device class, IORegistry-style. Cheap heuristic off the
 * parent bus / device name; iter 2 refines it from PCI/USB enrichment.
 */
static void
classify(struct hw_node *n)
{
	const struct hw_node *p = hwreg_get(n->parent_id);
	const char *pn = (p != NULL) ? p->name : "";

	if (strncmp(pn, "pci", 3) == 0)
		strlcpy(n->classname, "PCIDevice", sizeof(n->classname));
	else if (strncmp(pn, "usbus", 5) == 0 || strncmp(pn, "uhub", 4) == 0)
		strlcpy(n->classname, "USBDevice", sizeof(n->classname));
	else if (strncmp(n->name, "cpu", 3) == 0)
		strlcpy(n->classname, "CPU", sizeof(n->classname));
	else if (strncmp(n->name, "acpi", 4) == 0)
		strlcpy(n->classname, "ACPIDevice", sizeof(n->classname));
	else
		strlcpy(n->classname, "Device", sizeof(n->classname));
}

/*
 * Fill n->path = parent->path + "/" + name. Recurses parent-first so
 * the result is correct regardless of enumeration order, memoised by
 * the "already has a path" check; the device tree is acyclic so the
 * recursion terminates.
 */
static void
ensure_path(struct hw_node *n)
{
	struct hw_node *p;

	if (n->path[0] != '\0')
		return;
	p = hwreg_get(n->parent_id);
	if (p != NULL) {
		ensure_path(p);
		(void)snprintf(n->path, sizeof(n->path), "%s/%s",
		    p->path, n->name);
	} else {
		(void)snprintf(n->path, sizeof(n->path), "/%s", n->name);
	}
}

/* Transient parent-handle map, used only while building the snapshot. */
struct devent {
	uintptr_t	handle;
	uintptr_t	parent;
	struct hw_node *node;
};

/*
 * Return the field `*pp` points at and advance `*pp` past its NUL.
 * dv_fields holds five NUL-terminated strings packed back to back;
 * the caller forces a NUL at the buffer end so a malformed reply
 * still yields valid C strings and `*pp` can never pass `end`.
 */
static const char *
next_field(const char **pp, const char *end)
{
	const char *s = *pp;
	const char *q = s;

	while (q < end && *q != '\0')
		q++;
	*pp = (q < end) ? q + 1 : end;
	return s;
}

/* Free every node and reset the registry to empty — used between
 * snapshot retries when the kernel generation count moves. */
static void
registry_reset(void)
{
	struct hw_node *n = registry_head, *nx;

	while (n != NULL) {
		nx = n->next;
		free(n);
		n = nx;
	}
	registry_head = NULL;
	registry_tail = NULL;
	node_count = 0;
	next_id = 1;
}

/*
 * One attempt at building the registry from the newbus sysctls.
 * Returns the node count, or -1 with errno set; errno == EINVAL means
 * the kernel device generation moved mid-scan — the caller resets and
 * retries (the same contract libdevinfo's devinfo_init() works to).
 */
static int
snapshot_attempt(void)
{
	struct u_businfo ubus;
	struct devent *ents = NULL;
	size_t len, nents = 0, cap = 0, i, j;
	int mib[CTL_MAXNAME];
	size_t miblen = nitems(mib);
	int generation, idx;
	struct hw_node *n;

	/* hw.bus.info — validate the newbus userland ABI and get the
	 * generation count that tags every hw.bus.devices query. */
	len = sizeof(ubus);
	if (sysctlbyname("hw.bus.info", &ubus, &len, NULL, 0) != 0)
		return -1;
	if (len != sizeof(ubus) || ubus.ub_version != BUS_USER_VERSION) {
		errno = ENOTSUP;
		return -1;
	}
	generation = ubus.ub_generation;

	if (sysctlnametomib("hw.bus.devices", mib, &miblen) != 0)
		return -1;

	/*
	 * Pass A: one struct u_device per index — create a node, stash
	 * its handle + parent handle for the linkage pass. The device
	 * OID is hw.bus.devices + <generation> + <index>; a stale
	 * generation makes the kernel return EINVAL.
	 */
	for (idx = 0; ; idx++) {
		struct u_device ud;
		const char *p, *pend;
		const char *f_name, *f_desc, *f_drv, *f_pnp, *f_loc;

		mib[miblen] = generation;
		mib[miblen + 1] = idx;
		len = sizeof(ud);
		if (sysctl(mib, (u_int)(miblen + 2), &ud, &len, NULL, 0) != 0) {
			if (errno == ENOENT)
				break;		/* past the last device */
			free(ents);
			return -1;		/* EINVAL bubbles up as a retry */
		}
		if (len != sizeof(ud)) {
			free(ents);
			errno = EINVAL;
			return -1;
		}
		ud.dv_fields[sizeof(ud.dv_fields) - 1] = '\0';
		p = ud.dv_fields;
		pend = ud.dv_fields + sizeof(ud.dv_fields);
		f_name = next_field(&p, pend);
		f_desc = next_field(&p, pend);
		f_drv  = next_field(&p, pend);
		f_pnp  = next_field(&p, pend);
		f_loc  = next_field(&p, pend);

		n = node_new(f_name, 0);
		if (n == NULL) {
			free(ents);
			errno = ENOMEM;
			return -1;
		}
		strlcpy(n->desc, f_desc, sizeof(n->desc));
		strlcpy(n->driver, f_drv, sizeof(n->driver));
		strlcpy(n->pnpinfo, f_pnp, sizeof(n->pnpinfo));
		strlcpy(n->location, f_loc, sizeof(n->location));
		n->state = (ud.dv_state >= DS_ATTACHED) ? HW_ATTACHED : HW_PROBED;

		if (nents == cap) {
			size_t ncap = (cap == 0) ? 128 : cap * 2;
			struct devent *ne = realloc(ents, ncap * sizeof(*ne));

			if (ne == NULL) {
				free(ents);
				errno = ENOMEM;
				return -1;
			}
			ents = ne;
			cap = ncap;
		}
		ents[nents].handle = ud.dv_handle;
		ents[nents].parent = ud.dv_parent;
		ents[nents].node = n;
		nents++;
	}

	/* Pass B: resolve each device's parent handle to a registry id. */
	for (i = 0; i < nents; i++) {
		if (ents[i].parent == 0)
			continue;		/* root of the forest */
		for (j = 0; j < nents; j++) {
			if (j != i && ents[j].handle == ents[i].parent) {
				ents[i].node->parent_id = ents[j].node->id;
				break;
			}
		}
	}
	free(ents);

	/* Parents are linked now — derive class + path for every node. */
	for (n = registry_head; n != NULL; n = n->next) {
		classify(n);
		ensure_path(n);
	}
	return (int)nents;
}

int
hwreg_build_snapshot(void)
{
	int retries, r;

	/*
	 * Retry if the kernel device generation count moves mid-scan
	 * (snapshot_attempt() returns -1/EINVAL) — the registry built so
	 * far is stale, so discard it and rescan from a fresh generation.
	 */
	for (retries = 0; retries < 8; retries++) {
		r = snapshot_attempt();
		if (r >= 0)
			return r;
		if (errno != EINVAL)
			return -1;
		registry_reset();
	}
	errno = EAGAIN;
	return -1;
}

void
hwreg_note_attach(const char *name, const char *parent, const char *location)
{
	struct hw_node *n;

	if (name == NULL || name[0] == '\0')
		return;
	n = hwreg_find_by_name(name);
	if (n == NULL) {
		/* Hot-plugged device that was not in the boot snapshot. */
		struct hw_node *p = hwreg_find_by_name(parent);

		n = node_new(name, (p != NULL) ? p->id : 0);
		if (n == NULL)
			return;
		classify(n);
		ensure_path(n);
	}
	n->state = HW_ATTACHED;
	if (location != NULL && location[0] != '\0')
		strlcpy(n->location, location, sizeof(n->location));
}

void
hwreg_note_detach(const char *name)
{
	struct hw_node *n = hwreg_find_by_name(name);

	if (n != NULL)
		n->state = HW_DETACHED;
}
