/*
 * IOKitLib.c — libIOKit: read-only registry walk.
 *
 * Each registry entry point reads the kernel /dev/ioregistry device (the
 * K1 in-kernel registry, nextbsd#214) via ioctl. /dev/ioregistry walks the
 * live newbus device_t tree directly; the kernel is now the registry, so
 * there is no userland fallback — hwregd (#218) has been retired.
 *
 * Handles are client-side structs with a node id (or a captured id array
 * for iterators) and an atomic refcount. The /dev/ioregistry fd is
 * resolved lazily on first use under one pthread_once and cached
 * process-wide.
 */
#include <IOKit/IOKitLib.h>
#include "IOKitInternal.h"

#include "ioregistry.h"		/* vendored K1 ABI; canonical in nextbsd-kernel */

#include <mach/mach.h>

#include <sys/syscall.h>	/* NO_SYSCALL */
#ifndef NO_SYSCALL
#define NO_SYSCALL (-1)	/* kernel sentinel; sysctl reports -1 when a trap is unwired */
#endif
#include <sys/ioctl.h>		/* ioctl(2) on /dev/ioregistry */
#include <sys/sysctl.h>		/* sysctlbyname (mach.bus.busy / mach.syscall.*) */

#include <fcntl.h>		/* open(2), O_RDONLY / O_CLOEXEC */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>		/* syscall(2) */

/*
 * Lazy /dev/ioregistry resolution under one pthread_once:
 *   - ioregistry_fd: fd to /dev/ioregistry, or -1 if the K1 device is
 *     absent (a kernel image predating K1 — then the registry calls
 *     return kIOReturnNoDevice / IO_OBJECT_NULL).
 * The fd is process-lived and intentionally never closed (libIOKit is
 * a long-lived facade); O_CLOEXEC keeps it from leaking across exec.
 */
static pthread_once_t	io_backend_once = PTHREAD_ONCE_INIT;
static int		ioregistry_fd = -1;

static void
io_backend_init(void)
{
	ioregistry_fd = open("/dev/ioregistry", O_RDONLY | O_CLOEXEC);
}

int
__io_ioregistry_fd(void)
{
	(void)pthread_once(&io_backend_once, io_backend_init);
	return (ioregistry_fd);
}

/*
 * __io_node — fetch one node's scalar fields from /dev/ioregistry
 * (IOREGIOCNODE: fill struct ioreg_node.id, ioctl, copy out the
 * fields). Any out-pointer may be NULL.
 */
kern_return_t
__io_node(uint64_t node_id, uint64_t *parent_id, int *state,
    char name[32], char classname[32], char driver[32], char path[256])
{
	int fd = __io_ioregistry_fd();
	struct ioreg_node n;

	if (fd < 0)
		return (kIOReturnNoDevice);

	(void)memset(&n, 0, sizeof(n));
	n.id = node_id;
	if (ioctl(fd, IOREGIOCNODE, &n) != 0)
		return (kIOReturnNotFound);
	if (parent_id != NULL)
		*parent_id = n.parent_id;
	if (state != NULL)
		*state = n.state;
	if (name != NULL)
		(void)memcpy(name, n.name, IOREG_NAME_MAX);
	if (classname != NULL)
		(void)memcpy(classname, n.classname, IOREG_NAME_MAX);
	if (driver != NULL)
		(void)memcpy(driver, n.driver, IOREG_NAME_MAX);
	if (path != NULL)
		(void)memcpy(path, n.path, IOREG_PATH_MAX);
	return (KERN_SUCCESS);
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
	int fd = __io_ioregistry_fd();
	uint64_t root = 0;

	if (fd < 0)
		return (IO_OBJECT_NULL);
	if (ioctl(fd, IOREGIOCROOT, &root) != 0 || root == 0)
		return (IO_OBJECT_NULL);
	return (__io_alloc_entry(root));
}

/*
 * Fixed capacity for a single get-children call. The kernel reports the
 * true child count via ioreg_children.count; we clamp to this.
 */
#define IOKIT_MAX_CHILDREN	128

kern_return_t
IORegistryEntryGetChildIterator(io_registry_entry_t entry,
    const io_name_t plane __unused, io_iterator_t *iterator)
{
	int fd = __io_ioregistry_fd();
	uint64_t *ids;
	uint32_t cap = IOKIT_MAX_CHILDREN;

	if (entry == NULL || entry->kind != IOOBJ_KIND_ENTRY ||
	    iterator == NULL)
		return (KERN_INVALID_ARGUMENT);

	if (fd < 0)
		return (kIOReturnNoDevice);

	ids = calloc(cap, sizeof(*ids));
	if (ids == NULL)
		return (KERN_RESOURCE_SHORTAGE);

	{
		struct ioreg_children c;

		(void)memset(&c, 0, sizeof(c));
		c.id = entry->node_id;
		c.max = cap;
		c.children = (uint64_t)(uintptr_t)ids;
		if (ioctl(fd, IOREGIOCCHILDREN, &c) != 0) {
			free(ids);
			return (kIOReturnError);
		}
		/* Truncation: c.count > cap means more children than the
		 * fixed array holds; clamp to what we captured. */
		if (c.count > cap)
			c.count = cap;
		*iterator = __io_alloc_iterator(ids, c.count);
		return (*iterator != IO_OBJECT_NULL ? KERN_SUCCESS
		    : KERN_RESOURCE_SHORTAGE);
	}
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
	char hwname[32];
	kern_return_t kr;

	if (entry == NULL || entry->kind != IOOBJ_KIND_ENTRY ||
	    name == NULL)
		return (KERN_INVALID_ARGUMENT);
	/* Only the name is wanted; __io_node fills just that field and
	 * skips the rest (NULLs). The 32/256 buffers match the K1
	 * ioreg_node[name] bound. */
	kr = __io_node(entry->node_id, NULL, NULL, hwname, NULL, NULL, NULL);
	if (kr != KERN_SUCCESS)
		return (kr);
	(void)strlcpy(name, hwname, sizeof(io_name_t));
	return (KERN_SUCCESS);
}

kern_return_t
IORegistryEntryGetPath(io_registry_entry_t entry,
    const io_name_t plane __unused, io_string_t path)
{
	char hwpath[256];
	kern_return_t kr;

	if (entry == NULL || entry->kind != IOOBJ_KIND_ENTRY ||
	    path == NULL)
		return (KERN_INVALID_ARGUMENT);
	kr = __io_node(entry->node_id, NULL, NULL, NULL, NULL, NULL, hwpath);
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

/*
 * iter 5 — bus quiescence.
 *
 * Bus-busy state lives in the kernel
 * (mach.ko's device_match_start/device_match_end consumer), so we read
 * `sysctl mach.bus.busy` and resolve+call the `mach_wait_quiet` syscall
 * directly — the same lazy "resolve via sysctl mach.syscall.<name>,
 * cache the number" pattern libmach uses. See the header for the
 * GLOBAL-APPROXIMATION divergence from Apple (per-entry busy state is
 * reported host-wide).
 */

/* resolve_mach_syscall — read sysctl mach.syscall.<name> -> syscall #. */
static int
resolve_mach_syscall(const char *name)
{
	char oid[64];
	int num;
	size_t len = sizeof(num);

	if (snprintf(oid, sizeof(oid), "mach.syscall.%s", name) >=
	    (int)sizeof(oid))
		return (NO_SYSCALL);
	if (sysctlbyname(oid, &num, &len, NULL, 0) != 0)
		return (NO_SYSCALL);
	if (num < 0)
		return (NO_SYSCALL);
	return (num);
}

kern_return_t
IORegistryEntryGetBusyState(mach_port_t mainPort __unused,
    io_registry_entry_t entry __unused, uint32_t *busyState)
{
	int busy = 0;
	size_t len = sizeof(busy);

	if (busyState == NULL)
		return (KERN_INVALID_ARGUMENT);
	/* `entry` ignored — host-wide approximation (see header). */
	if (sysctlbyname("mach.bus.busy", &busy, &len, NULL, 0) != 0)
		return (kIOReturnError);
	*busyState = (uint32_t)busy;
	return (kIOReturnSuccess);
}

/*
 * __io_wait_quiet_ns — resolve+invoke mach_wait_quiet with a nanosecond
 * budget (0 == wait indefinitely). Returns kIOReturnSuccess on quiesce
 * or deadline; kIOReturnError if the syscall can't be resolved.
 */
static IOReturn
__io_wait_quiet_ns(uint64_t timeout_ns)
{
	static int num = NO_SYSCALL;

	if (num == NO_SYSCALL) {
		num = resolve_mach_syscall("mach_wait_quiet");
		if (num == NO_SYSCALL)
			return (kIOReturnError);
	}
	if (syscall(num, timeout_ns) != 0)
		return (kIOReturnError);
	return (kIOReturnSuccess);
}

/* mach_timespec_t -> nanoseconds; NULL == 0 == wait indefinitely. */
static uint64_t
__io_timespec_to_ns(mach_timespec_t *ts)
{
	if (ts == NULL)
		return (0);
	return ((uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec);
}

IOReturn
IOKitWaitQuiet(mach_port_t mainPort __unused, mach_timespec_t *timeout)
{
	return (__io_wait_quiet_ns(__io_timespec_to_ns(timeout)));
}

kern_return_t
IOServiceWaitQuiet(io_service_t service __unused, mach_timespec_t *timeout)
{
	/* `service` ignored — host-wide approximation (see header). */
	return (__io_wait_quiet_ns(__io_timespec_to_ns(timeout)));
}
