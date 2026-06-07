/*
 * iokitnotifyrt — libIOKit notification round-trip test (C1.2, #218).
 *
 * Proves the FULL device-event path the C1.2 migration moved onto the kernel
 * notify channel: register an IOServiceAddMatchingNotification through libIOKit
 * (which, when /dev/ioregistry is present, registers the recv port via
 * IOREGIOCWATCH on the in-kernel registry, #225), then SYNTHESIZE a matching
 * device-arrival event with the IOREGIOCTESTEVENT inject ioctl (kernel PR,
 * #225/#218) and confirm the registered callback actually fires. This exercises
 * the kernel match + ioreg_event_msg Mach send + the receive thread's binary
 * decode end-to-end, deterministically, with no physical device.
 *
 * SELF-SKIP (never a hard failure) when the round-trip cannot be staged:
 *   - no /dev/ioregistry            -> kernel predating K1 (libIOKit on hwregd)
 *   - IOREGIOCTESTEVENT == ENOTTY   -> kernel predating the Part A inject ioctl
 *   - IOREGIOCTESTEVENT == ENOSYS   -> kernel built without COMPAT_MACH
 * This keeps the gate CI-greenable on a continuous image built before the Part A
 * kernel reaches continuous (the kernel PR must merge + ingest first).
 *
 * Emits EXACTLY ONE marker (gated in tests/boot-test.sh):
 *   IOKITNOTIFY-OK    — injected event arrived at the registered callback
 *   IOKITNOTIFY-FAIL  — channel present but the callback never fired
 *   IOKITNOTIFY-SKIP  — round-trip not stageable on this kernel (see above)
 */
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>

#include "ioregistry.h"		/* vendored ABI: IOREGIOCTESTEVENT etc. */

#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A class string no real newbus device carries, so the only event that can ever
 * match this watch is the one we inject — no false positives from real attaches
 * racing the test. */
#define	RT_TEST_NAME	"nbiokit_rt_probe"

static atomic_int g_fired;

static void
rt_callback(void *refcon __unused, io_iterator_t iterator)
{
	io_object_t obj;

	/* Drain the iterator the facade handed us (it owns the synthetic id). */
	while ((obj = IOIteratorNext(iterator)) != IO_OBJECT_NULL)
		IOObjectRelease(obj);
	atomic_store(&g_fired, 1);
}

int
main(void)
{
	IONotificationPortRef	notify;
	CFMutableDictionaryRef	matching;
	io_iterator_t		it = IO_OBJECT_NULL;
	io_object_t		obj;
	struct ioreg_test_event	te;
	int			fd, i;

	fd = open("/dev/ioregistry", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		printf("IOKITNOTIFY-SKIP: no /dev/ioregistry "
		    "(kernel without the K1 in-kernel registry)\n");
		return (0);
	}

	/* Probe IOREGIOCTESTEVENT before staging the watch: a kind of 0 is
	 * rejected with EINVAL by a kernel that HAS the ioctl, and with ENOTTY
	 * by one that lacks it (the cdev's default case). ENOSYS means the
	 * kernel has the ioctl but no COMPAT_MACH channel. Either absence -> a
	 * non-fatal SKIP so this gate is green on pre-Part-A continuous images. */
	memset(&te, 0, sizeof(te));
	te.kind = 0;
	if (ioctl(fd, IOREGIOCTESTEVENT, &te) != 0 &&
	    (errno == ENOTTY || errno == ENOSYS)) {
		printf("IOKITNOTIFY-SKIP: IOREGIOCTESTEVENT absent (errno=%d) "
		    "— kernel predating the notify test-inject ioctl\n", errno);
		(void)close(fd);
		return (0);
	}

	atomic_store(&g_fired, 0);

	notify = IONotificationPortCreate(kIOMainPortDefault);
	if (notify == NULL) {
		printf("IOKITNOTIFY-FAIL: IONotificationPortCreate\n");
		(void)close(fd);
		return (1);
	}

	/* Match on a unique name only we will inject (IONameMatch -> the kernel
	 * criteria "name" key, AND-matched against the injected node's name). */
	matching = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (matching == NULL) {
		printf("IOKITNOTIFY-FAIL: CFDictionaryCreateMutable\n");
		IONotificationPortDestroy(notify);
		(void)close(fd);
		return (1);
	}
	{
		CFStringRef v = CFStringCreateWithCString(kCFAllocatorDefault,
		    RT_TEST_NAME, kCFStringEncodingUTF8);

		CFDictionarySetValue(matching, CFSTR(kIONameMatchKey), v);
		CFRelease(v);
	}

	if (IOServiceAddMatchingNotification(notify, kIOFirstMatchNotification,
	    matching, rt_callback, NULL, &it) != KERN_SUCCESS) {
		printf("IOKITNOTIFY-FAIL: AddMatchingNotification\n");
		IONotificationPortDestroy(notify);
		(void)close(fd);
		return (1);
	}
	/* `matching` consumed by the facade. Drain the (expected empty) initial
	 * arming — no real device carries RT_TEST_NAME. */
	if (it != IO_OBJECT_NULL) {
		while ((obj = IOIteratorNext(it)) != IO_OBJECT_NULL)
			IOObjectRelease(obj);
		IOObjectRelease(it);
	}

	/* Inject the matching arrival event through the same match+send path a
	 * real device_attach takes. The receive thread should decode the
	 * binary ioreg_event_msg, re-match RT_TEST_NAME, and fire rt_callback. */
	memset(&te, 0, sizeof(te));
	te.kind = IOREG_EVENT_ARRIVE;
	te.id = 0x6e62696fULL;	/* arbitrary synthetic registry id */
	strlcpy(te.name, RT_TEST_NAME, sizeof(te.name));
	te.classname[0] = '\0';
	te.pci_vendor = 0;
	te.pci_device = 0;
	if (ioctl(fd, IOREGIOCTESTEVENT, &te) != 0) {
		/* The probe above already ruled out absence; a failure here is a
		 * real channel error. */
		printf("IOKITNOTIFY-FAIL: IOREGIOCTESTEVENT inject failed "
		    "(errno=%d)\n", errno);
		IONotificationPortDestroy(notify);
		(void)close(fd);
		return (1);
	}

	/* Wait for the callback. The receive thread polls on a 500ms mach_msg
	 * timeout, so allow comfortably more than one cycle. */
	for (i = 0; i < 50 && atomic_load(&g_fired) == 0; i++)
		usleep(100 * 1000);	/* 100ms * 50 = 5s budget */

	IONotificationPortDestroy(notify);
	(void)close(fd);

	if (atomic_load(&g_fired) != 0) {
		printf("IOKITNOTIFY-OK: injected device event round-tripped "
		    "through IOREGIOCWATCH to the registered callback\n");
		return (0);
	}
	printf("IOKITNOTIFY-FAIL: injected event never reached the callback\n");
	return (1);
}
