/*
 * da_iokit_subscribe.c — DiskArbitration kernel-notify subscription (C1.3, #218).
 *
 * Migrates DA's device-arrival/removal subscription off hwregd's raw pub/sub
 * onto the kernel notify channel, reusing libIOKit's now-proven
 * IOServiceAddMatchingNotification path (#241, IOKITNOTIFY-OK): an
 * IONotificationPort whose recv port libIOKit registers with the in-kernel
 * registry via IOREGIOCWATCH on /dev/ioregistry, the kernel pushing a binary
 * ioreg_event_msg per matching event into libIOKit's receive thread, which fires
 * our IOServiceMatchingCallback (delivered on DA's dispatch queue via
 * IONotificationPortSetDispatchQueue).
 *
 * DA cares about MANY storage classes (da/ada/nvd/nda/cd/mmcsd), but the kernel
 * watch criteria is a single flat by-value struct that matches ONE class or a
 * wildcard. So DA registers a MATCH-ALL notification (empty matching dictionary
 * leaves every kernel criteria field zeroed, which the kernel ABI treats as a
 * wildcard) for both arrival (kIOFirstMatchNotification) and departure
 * (kIOTerminatedNotification), and keeps the is_storage_name() filter
 * client-side in the callback — exactly the receive-all-then-filter shape
 * hwreg_subscribe.c uses today.
 *
 * FALLBACK: this path needs /dev/ioregistry. If it is absent (a kernel image
 * predating the K1 in-kernel registry), da_iokit_subscribe_start() returns -1
 * (without a failure marker) and diskarbitrationd falls back to the legacy
 * hwreg_subscribe_start() pub/sub path, which stays compiled (PR7/#218 removes
 * it). libIOKit itself also has an internal hwregd fallback inside
 * IONotificationPortCreate, but DA gates on /dev/ioregistry up front so a
 * fallback run takes the SAME hwregd subscription DA used before this change,
 * and so the DA-IOKIT gate can self-SKIP cleanly (rather than double-subscribing
 * via two different hwregd clients).
 */
#include "da_iokit_subscribe.h"

#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * One IONotificationPort serves both the arrival and the departure watch; the
 * dispatch queue delivers their callbacks. Kept process-global (the daemon owns
 * exactly one subscription for its lifetime) so the receive thread + queue
 * outlive da_iokit_subscribe_start(). There is no teardown path: the daemon
 * holds the subscription until exit, same as hwreg_subscribe's recv thread.
 */
static IONotificationPortRef	g_notify;
static dispatch_queue_t		g_queue;

static void
xlog(const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "diskarbitrationd[iokit] ");
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/*
 * True if `name` looks like a CAM storage peripheral DA cares about: ada*
 * (AHCI/CAM ATA), da* (SCSI / USB), nvd* / nda* (NVMe), cd* (optical), mmcsd*
 * (SD/eMMC). Same rule (and same prefix list) as hwreg_subscribe.c's
 * is_storage_name — kept in sync so both paths filter CAM peripheral names
 * identically. These are the disk-layer names hwregd's pub/sub events carry.
 */
static int
is_storage_name(const char *name)
{
	static const char *prefixes[] = {
		"ada", "da", "nvd", "nda", "cd", "mmcsd"
	};
	size_t i;

	for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		size_t plen = strlen(prefixes[i]);
		if (strncmp(name, prefixes[i], plen) != 0)
			continue;
		if (name[plen] >= '0' && name[plen] <= '9')
			return (1);
	}
	return (0);
}

/*
 * The in-kernel registry (#225) exposes newbus nodes by device_get_name() /
 * devclass — i.e. the BUS-LEVEL storage driver name (vtblk, ahci, ahcich,
 * nvme), NOT the CAM peripheral name (da0, ada0) that hwregd's higher-level
 * pub/sub events carry. is_storage_name above never matches a bus-driver name
 * (no trailing unit digit, not in its prefix list), so this companion check
 * recognizes the storage CONTROLLER/DISK driver names the registry actually
 * reports. Both the name and the class are tested against this set (the registry
 * fills both fields, and under qemu the virtio-blk disk shows up as name/class
 * "vtblk"; AHCI as "ahci"/"ahcich"). Matching here is exact-or-prefix on the
 * known storage driver tokens so a non-storage driver can't false-positive.
 */
static int
is_storage_driver(const char *s)
{
	static const char *drivers[] = {
		"vtblk",	/* virtio-blk (qemu default disk) */
		"virtio_blk",	/* alternate virtio-blk devclass spelling */
		"ahcich",	/* per-channel AHCI (the disk-bearing node) */
		"ahci",		/* AHCI controller */
		"nvme",		/* NVMe controller */
		"mmc",		/* SD/eMMC host */
		"umass"		/* USB mass storage */
	};
	size_t i;

	if (s == NULL || s[0] == '\0')
		return (0);
	for (i = 0; i < sizeof(drivers) / sizeof(drivers[0]); i++)
		if (strncmp(s, drivers[i], strlen(drivers[i])) == 0)
			return (1);
	return (0);
}

/*
 * Storage decision for one registry node: true if either the CAM peripheral
 * name rule (is_storage_name) or the bus-level storage driver rule
 * (is_storage_driver, on name OR class) matches. This is what lets the qemu
 * vtblk boot disk — which the registry reports as name/class "vtblk", not
 * "da0" — drive DA's storage path through the kernel channel.
 */
static int
node_is_storage(const char *name, const char *klass)
{
	if (name[0] != '\0' && is_storage_name(name))
		return (1);
	if (is_storage_driver(name) || is_storage_driver(klass))
		return (1);
	return (0);
}

/*
 * Drive DA's arrival/departure path for one matched storage device. iter 2's
 * surface is a log line (the same observable hwreg_subscribe emits) plus the
 * distinct DA-IOKIT marker the boot test greps to prove the event came through
 * /dev/ioregistry. Later iters hang libgeom enrichment + the DiskArbitration
 * framework callbacks off this point, exactly where the hwregd path fed them.
 */
static void
da_handle_storage(const char *verb, const char *name, const char *klass)
{
	xlog("STORAGE %s name=%s class=%s (via /dev/ioregistry)",
	    verb, name, klass[0] != '\0' ? klass : "?");
	/*
	 * One-shot, unambiguous marker for the CI boot-test gate: storage
	 * arrival seen through the kernel notify channel. Emitting it on the
	 * ARRIVAL verb (the common qemu vtblk/ahci case) keeps the gate
	 * deterministic; departures still log above but do not re-fire the
	 * marker so the gate's expect block stays single-shot.
	 */
	if (strcmp(verb, "arrival") == 0)
		xlog("DA-IOKIT: storage %s %s via /dev/ioregistry", name,
		    klass[0] != '\0' ? klass : "");
}

/*
 * Drain the iterator libIOKit handed us, reading each node's name + class via
 * libIOKit and applying the storage filter. The facade owns the iterator (it
 * tears it down when we return), so we MUST NOT release it; we DO release each
 * io_object_t IOIteratorNext hands back. `verb` distinguishes the arrival watch
 * from the departure watch (the two share this drain).
 */
static void
drain_iterator(io_iterator_t it, const char *verb)
{
	io_object_t obj;

	while ((obj = IOIteratorNext(it)) != IO_OBJECT_NULL) {
		io_name_t name;
		io_name_t klass;

		name[0] = '\0';
		klass[0] = '\0';
		(void)IORegistryEntryGetName(obj, name);
		(void)IOObjectGetClass(obj, klass);

		if (node_is_storage(name, klass))
			da_handle_storage(verb, name, klass);
		else
			xlog("event %s name=%s class=%s (non-storage, ignored)",
			    verb, name[0] != '\0' ? name : "?",
			    klass[0] != '\0' ? klass : "?");

		IOObjectRelease(obj);
	}
}

static void
arrival_callback(void *refcon __unused, io_iterator_t it)
{
	drain_iterator(it, "arrival");
}

static void
departure_callback(void *refcon __unused, io_iterator_t it)
{
	drain_iterator(it, "departure");
}

/*
 * Build a MATCH-ALL matching dictionary: an empty CFDictionary. libIOKit's
 * __io_extract_criteria leaves every io_criteria field empty, __io_fill_criteria
 * then zeroes the kernel struct, and the kernel ABI treats an all-zero criteria
 * as "match every node" — so the watch fires for every storage class without
 * enumerating them. (IOServiceAddMatchingNotification consumes one reference of
 * the dictionary, so each watch gets its own fresh copy.)
 */
static CFMutableDictionaryRef
match_all_dict(void)
{
	return (CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
}

int
da_iokit_subscribe_start(void)
{
	CFMutableDictionaryRef arrive_match, depart_match;
	io_iterator_t arrive_it = IO_OBJECT_NULL;
	io_iterator_t depart_it = IO_OBJECT_NULL;
	io_object_t obj;
	kern_return_t kr;

	/*
	 * Gate on /dev/ioregistry up front. If it is absent, the kernel notify
	 * channel does not exist on this image; return -1 (no failure marker)
	 * so the caller takes the legacy hwregd fallback. (libIOKit would itself
	 * fall back to hwregd internally, but DA prefers a single explicit
	 * decision here so the fallback uses DA's own hwreg_subscribe client and
	 * the DA-IOKIT gate can self-SKIP rather than report a half-kernel path.)
	 */
	{
		int fd = open("/dev/ioregistry", O_RDONLY | O_CLOEXEC);

		if (fd < 0) {
			xlog("no /dev/ioregistry — kernel notify channel "
			    "unavailable; falling back to hwregd pub/sub");
			return (-1);
		}
		(void)close(fd);
	}

	g_notify = IONotificationPortCreate(kIOMainPortDefault);
	if (g_notify == NULL) {
		xlog("DA-IOKIT-REGFAIL: IONotificationPortCreate failed");
		return (-1);
	}

	/*
	 * Deliver callbacks on a serial DA dispatch queue, mirroring how DA
	 * consumes events (libdispatch). The receive thread inside libIOKit
	 * dispatch_async_f's each callback onto this queue, so device events
	 * are serialized w.r.t. each other on DA's own queue rather than
	 * running on libIOKit's private receive thread.
	 */
	g_queue = dispatch_queue_create("com.apple.DiskArbitration.iokit", NULL);
	if (g_queue == NULL) {
		xlog("DA-IOKIT-REGFAIL: dispatch_queue_create failed");
		IONotificationPortDestroy(g_notify);
		g_notify = NULL;
		return (-1);
	}
	IONotificationPortSetDispatchQueue(g_notify, g_queue);

	/* Arrival watch (match-all; storage filtered client-side). */
	arrive_match = match_all_dict();
	if (arrive_match == NULL) {
		xlog("DA-IOKIT-REGFAIL: match dict alloc (arrival)");
		goto fail;
	}
	kr = IOServiceAddMatchingNotification(g_notify,
	    kIOFirstMatchNotification, arrive_match, arrival_callback, NULL,
	    &arrive_it);
	if (kr != KERN_SUCCESS) {
		xlog("DA-IOKIT-REGFAIL: AddMatchingNotification(arrival) kr=0x%x",
		    (unsigned)kr);
		goto fail;
	}

	/* Departure watch. */
	depart_match = match_all_dict();
	if (depart_match == NULL) {
		xlog("DA-IOKIT-REGFAIL: match dict alloc (departure)");
		goto fail;
	}
	kr = IOServiceAddMatchingNotification(g_notify,
	    kIOTerminatedNotification, depart_match, departure_callback, NULL,
	    &depart_it);
	if (kr != KERN_SUCCESS) {
		xlog("DA-IOKIT-REGFAIL: AddMatchingNotification(departure) kr=0x%x",
		    (unsigned)kr);
		goto fail;
	}

	xlog("DA-IOKIT-ARMED: subscribed to kernel device notifications via "
	    "/dev/ioregistry (match-all arrival + departure, storage filtered "
	    "client-side)");

	/*
	 * Initial arming: each watch handed back an iterator over the devices
	 * that ALREADY match (Apple's contract). Drain them so storage devices
	 * present at boot are reported through the SAME path as later arrivals —
	 * this is what makes the qemu vtblk disk, attached before DA starts,
	 * fire the DA-IOKIT marker. Departure's initial set is also drained
	 * (logged, but it does not re-fire the marker). The facade does NOT tear
	 * these iterators down (they are the caller-owned arming iterators, not
	 * the per-event ones), so we release them ourselves.
	 */
	drain_iterator(arrive_it, "arrival");
	drain_iterator(depart_it, "departure");
	IOObjectRelease(arrive_it);
	IOObjectRelease(depart_it);

	return (0);

fail:
	if (arrive_it != IO_OBJECT_NULL) {
		while ((obj = IOIteratorNext(arrive_it)) != IO_OBJECT_NULL)
			IOObjectRelease(obj);
		IOObjectRelease(arrive_it);
	}
	if (depart_it != IO_OBJECT_NULL) {
		while ((obj = IOIteratorNext(depart_it)) != IO_OBJECT_NULL)
			IOObjectRelease(obj);
		IOObjectRelease(depart_it);
	}
	IONotificationPortDestroy(g_notify);
	g_notify = NULL;
	if (g_queue != NULL) {
		dispatch_release(g_queue);
		g_queue = NULL;
	}
	return (-1);
}
