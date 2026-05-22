/*
 * iokitnotifytest — libIOKit iter 4 notification-wiring test.
 *
 * Validates the IONotificationPort + IOServiceAddMatchingNotification
 * surface end-to-end (ALLOC port → SetDispatchQueue → register a
 * Match notification → drain the initial-arming iterator → tear
 * down). The async device-arrival fire path is NOT directly tested
 * in CI — QEMU device hot-plug isn't injectable from the boot test,
 * and the underlying raw-mach_msg receive thread is structurally
 * identical to HWREG-PUBSUB / SC-NOTIFY (both CI-proven). Once a
 * real consumer (IPConfiguration / dhclient replacement) exercises
 * the async path on real link-up events, that's the live proof.
 *
 * Marker IOKIT-NOTIFY gates in tests/boot-test.sh.
 */
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>

#include <stdio.h>
#include <stdlib.h>

static void
arrival_callback(void *refcon __unused, io_iterator_t iterator __unused)
{
	/* iter-4 test never fires this in CI — see file header. The
	 * function exists so AddMatchingNotification has a real
	 * function pointer to record. */
}

int
main(void)
{
	IONotificationPortRef	notify;
	mach_port_t		mp;
	dispatch_queue_t	q;
	CFMutableDictionaryRef	matching;
	io_iterator_t		it = IO_OBJECT_NULL;
	io_object_t		obj;
	int			matched = 0;

	notify = IONotificationPortCreate(kIOMainPortDefault);
	if (notify == NULL) {
		printf("IOKIT-NOTIFY-FAIL: IONotificationPortCreate\n");
		return (1);
	}
	mp = IONotificationPortGetMachPort(notify);
	if (mp == MACH_PORT_NULL) {
		printf("IOKIT-NOTIFY-FAIL: IONotificationPortGetMachPort "
		    "returned MACH_PORT_NULL\n");
		IONotificationPortDestroy(notify);
		return (1);
	}
	printf("  IONotificationPort recv port = 0x%x\n", (unsigned)mp);

	q = dispatch_queue_create("org.freebsd.iokit.notifytest", NULL);
	if (q == NULL) {
		printf("IOKIT-NOTIFY-FAIL: dispatch_queue_create\n");
		IONotificationPortDestroy(notify);
		return (1);
	}
	IONotificationPortSetDispatchQueue(notify, q);
	dispatch_release(q);	/* port retains one ref */

	/* Register an arrival notification for PCIDevice — initial
	 * arming should hand back ≥1 entry (i440FX hostb0 + the e1000
	 * NIC are always present in the QEMU guest). */
	matching = IOServiceMatching("PCIDevice");
	if (matching == NULL) {
		printf("IOKIT-NOTIFY-FAIL: IOServiceMatching\n");
		IONotificationPortDestroy(notify);
		return (1);
	}
	if (IOServiceAddMatchingNotification(notify,
	    kIOFirstMatchNotification, matching, arrival_callback, NULL,
	    &it) != KERN_SUCCESS || it == IO_OBJECT_NULL) {
		printf("IOKIT-NOTIFY-FAIL: AddMatchingNotification\n");
		IONotificationPortDestroy(notify);
		return (1);
	}
	/* `matching` has been consumed. */

	while ((obj = IOIteratorNext(it)) != IO_OBJECT_NULL) {
		io_name_t name;

		if (IORegistryEntryGetName(obj, name) == KERN_SUCCESS &&
		    matched < 3)
			printf("  initial arming[%d] = %s\n", matched, name);
		matched++;
		IOObjectRelease(obj);
	}
	IOObjectRelease(it);
	if (matched == 0) {
		printf("IOKIT-NOTIFY-FAIL: initial arming yielded 0 matches "
		    "(expected ≥1 PCIDevice)\n");
		IONotificationPortDestroy(notify);
		return (1);
	}

	/* Departure notification with a class no node carries — initial
	 * arming must yield an empty (but non-NULL) iterator. */
	matching = IOServiceMatching("ThisClassDoesNotExist");
	if (IOServiceAddMatchingNotification(notify,
	    kIOTerminatedNotification, matching, arrival_callback, NULL,
	    &it) != KERN_SUCCESS || it == IO_OBJECT_NULL) {
		printf("IOKIT-NOTIFY-FAIL: AddMatchingNotification(terminated)\n");
		IONotificationPortDestroy(notify);
		return (1);
	}
	if (IOIteratorNext(it) != IO_OBJECT_NULL) {
		printf("IOKIT-NOTIFY-FAIL: empty-class initial arming "
		    "yielded an entry\n");
		IOObjectRelease(it);
		IONotificationPortDestroy(notify);
		return (1);
	}
	IOObjectRelease(it);

	IONotificationPortDestroy(notify);
	printf("IOKIT-NOTIFY-OK: IONotificationPort + AddMatching"
	    "Notification wire up (%d PCIDevice initial match%s)\n",
	    matched, matched == 1 ? "" : "es");
	return (0);
}
