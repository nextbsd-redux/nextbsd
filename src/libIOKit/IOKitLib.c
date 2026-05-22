/*
 * IOKitLib.c — libIOKit iter 1: read-only registry walk.
 *
 * Each entry point is a thin wrapper over hwregd's MIG RPC
 * (src/hwregd/hwreg.defs). Handles are client-side structs with a
 * hwregd node id (or a captured id array for iterators) and an
 * atomic refcount. The hwregd service port is looked up lazily via
 * bootstrap_look_up on first use and cached process-wide.
 */
#include <IOKit/IOKitLib.h>
#include "IOKitInternal.h"

#include "hwreg.h"		/* MIG hwreg.defs user-side stubs */
#include "hwreg_mig_types.h"	/* hwreg_id_array_t / hwreg_name_t / ... */

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* hwregd send right — cached after the first bootstrap_look_up. */
static pthread_once_t	hwregd_once = PTHREAD_ONCE_INIT;
static mach_port_t	hwregd_port = MACH_PORT_NULL;

static void
hwregd_lookup(void)
{
	if (bootstrap_look_up(bootstrap_port,
	    "org.freebsd.hwregd", &hwregd_port) != KERN_SUCCESS)
		hwregd_port = MACH_PORT_NULL;
}

mach_port_t
__io_hwregd_port(void)
{
	(void)pthread_once(&hwregd_once, hwregd_lookup);
	return (hwregd_port);
}

io_object_t
__io_alloc_entry(uint64_t node_id)
{
	struct __IOObject *o = calloc(1, sizeof(*o));

	if (o == NULL)
		return (IO_OBJECT_NULL);
	o->kind = IOOBJ_KIND_ENTRY;
	atomic_store(&o->refcnt, 1);
	o->node_id = node_id;
	return (o);
}

io_object_t
__io_alloc_iterator(uint64_t *ids, uint32_t count)
{
	struct __IOObject *o = calloc(1, sizeof(*o));

	if (o == NULL) {
		free(ids);
		return (IO_OBJECT_NULL);
	}
	o->kind = IOOBJ_KIND_ITERATOR;
	atomic_store(&o->refcnt, 1);
	o->ids = ids;
	o->count = count;
	o->cursor = 0;
	return (o);
}

io_registry_entry_t
IORegistryGetRootEntry(mach_port_t mainPort __unused)
{
	mach_port_t svc = __io_hwregd_port();
	uint64_t root = 0;

	if (svc == MACH_PORT_NULL)
		return (IO_OBJECT_NULL);
	if (hwreg_get_root(svc, &root) != KERN_SUCCESS || root == 0)
		return (IO_OBJECT_NULL);
	return (__io_alloc_entry(root));
}

/*
 * hwreg.defs caps a single get_children reply at 128 ids — see the
 * hwreg_id_array_t bound. Mirror it here.
 */
#define IOKIT_MAX_CHILDREN	128

kern_return_t
IORegistryEntryGetChildIterator(io_registry_entry_t entry,
    const io_name_t plane __unused, io_iterator_t *iterator)
{
	mach_port_t svc = __io_hwregd_port();
	uint64_t *ids;
	mach_msg_type_number_t nids = IOKIT_MAX_CHILDREN;
	kern_return_t kr;

	if (entry == NULL || entry->kind != IOOBJ_KIND_ENTRY ||
	    iterator == NULL)
		return (KERN_INVALID_ARGUMENT);
	if (svc == MACH_PORT_NULL)
		return (kIOReturnNoDevice);

	ids = calloc(nids, sizeof(*ids));
	if (ids == NULL)
		return (KERN_RESOURCE_SHORTAGE);

	kr = hwreg_get_children(svc, entry->node_id, ids, &nids);
	if (kr != KERN_SUCCESS) {
		free(ids);
		return (kr);
	}
	*iterator = __io_alloc_iterator(ids, nids);
	return (*iterator != IO_OBJECT_NULL ? KERN_SUCCESS
	    : KERN_RESOURCE_SHORTAGE);
}

io_object_t
IOIteratorNext(io_iterator_t iterator)
{
	if (iterator == NULL || iterator->kind != IOOBJ_KIND_ITERATOR)
		return (IO_OBJECT_NULL);
	if (iterator->cursor >= iterator->count)
		return (IO_OBJECT_NULL);
	return (__io_alloc_entry(iterator->ids[iterator->cursor++]));
}

kern_return_t
IORegistryEntryGetName(io_registry_entry_t entry, io_name_t name)
{
	mach_port_t svc = __io_hwregd_port();
	uint64_t parent_id;
	int state;
	/* hwreg.defs bounds: hwreg_name_t = c_string[32], hwreg_path_t
	 * = c_string[256]. Mirror exactly so MIG fills them safely. */
	char hwname[32], classname[32], driver[32], path[256];
	kern_return_t kr;

	if (entry == NULL || entry->kind != IOOBJ_KIND_ENTRY ||
	    name == NULL)
		return (KERN_INVALID_ARGUMENT);
	if (svc == MACH_PORT_NULL)
		return (kIOReturnNoDevice);
	kr = hwreg_get_node(svc, entry->node_id, &parent_id, &state,
	    hwname, classname, driver, path);
	if (kr != KERN_SUCCESS)
		return (kr);
	(void)strlcpy(name, hwname, sizeof(io_name_t));
	return (KERN_SUCCESS);
}

kern_return_t
IORegistryEntryGetPath(io_registry_entry_t entry,
    const io_name_t plane __unused, io_string_t path)
{
	mach_port_t svc = __io_hwregd_port();
	uint64_t parent_id;
	int state;
	char name[32], classname[32], driver[32], hwpath[256];
	kern_return_t kr;

	if (entry == NULL || entry->kind != IOOBJ_KIND_ENTRY ||
	    path == NULL)
		return (KERN_INVALID_ARGUMENT);
	if (svc == MACH_PORT_NULL)
		return (kIOReturnNoDevice);
	kr = hwreg_get_node(svc, entry->node_id, &parent_id, &state,
	    name, classname, driver, hwpath);
	if (kr != KERN_SUCCESS)
		return (kr);
	/* Apple format: "<plane>:<components>". One plane → IOService. */
	(void)snprintf(path, sizeof(io_string_t), "IOService:%s", hwpath);
	return (KERN_SUCCESS);
}

kern_return_t
IOObjectRetain(io_object_t object)
{
	if (object == NULL)
		return (KERN_INVALID_ARGUMENT);
	(void)atomic_fetch_add(&object->refcnt, 1);
	return (KERN_SUCCESS);
}

kern_return_t
IOObjectRelease(io_object_t object)
{
	int prev;

	if (object == NULL)	/* releasing NULL is a no-op (macOS) */
		return (KERN_SUCCESS);
	prev = atomic_fetch_sub(&object->refcnt, 1);
	if (prev != 1)
		return (KERN_SUCCESS);
	if (object->kind == IOOBJ_KIND_ITERATOR)
		free(object->ids);
	free(object);
	return (KERN_SUCCESS);
}
