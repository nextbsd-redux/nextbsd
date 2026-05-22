/*
 * iokittest — libIOKit iter 1 test client. Walks the hwregd registry
 * via the IOKit facade — IORegistryGetRootEntry / GetChildIterator /
 * IOIteratorNext / GetName / GetPath — and prints IOKIT-WALK-OK on
 * success. Same shape as hwregquery (the direct-MIG client) but
 * goes through the facade. Boot marker IOKIT-WALK gates in
 * tests/boot-test.sh.
 */
#include <IOKit/IOKitLib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int	walked;
static int	failed;

static void
walk(io_registry_entry_t entry, int depth)
{
	io_name_t	name;
	io_string_t	path;
	io_iterator_t	it = IO_OBJECT_NULL;
	io_object_t	child;

	if (IORegistryEntryGetName(entry, name) != KERN_SUCCESS) {
		printf("IOKIT-WALK-FAIL: IORegistryEntryGetName at depth %d\n",
		    depth);
		failed = 1;
		return;
	}
	walked++;
	if (depth < 3) {
		if (IORegistryEntryGetPath(entry, "IOService", path) !=
		    KERN_SUCCESS)
			(void)strlcpy(path, "?", sizeof(path));
		printf("  %*s%s [%s]\n", depth * 2, "", name, path);
	}

	if (IORegistryEntryGetChildIterator(entry, "IOService", &it) !=
	    KERN_SUCCESS) {
		printf("IOKIT-WALK-FAIL: GetChildIterator at %s\n", name);
		failed = 1;
		return;
	}
	while ((child = IOIteratorNext(it)) != IO_OBJECT_NULL) {
		walk(child, depth + 1);
		IOObjectRelease(child);
	}
	IOObjectRelease(it);
}

int
main(void)
{
	io_registry_entry_t root;

	root = IORegistryGetRootEntry(kIOMainPortDefault);
	if (root == IO_OBJECT_NULL) {
		printf("IOKIT-WALK-FAIL: IORegistryGetRootEntry returned "
		    "IO_OBJECT_NULL (hwregd unreachable?)\n");
		return (1);
	}

	walk(root, 0);

	/* IOObjectRetain/Release round-trip on a live handle. */
	if (IOObjectRetain(root) != KERN_SUCCESS) {
		printf("IOKIT-WALK-FAIL: IOObjectRetain failed\n");
		IOObjectRelease(root);
		return (1);
	}
	IOObjectRelease(root);	/* drop the retain */
	IOObjectRelease(root);	/* drop the original */

	if (failed) {
		printf("IOKIT-WALK-FAIL: tree walk hit an error\n");
		return (1);
	}
	printf("IOKIT-WALK-OK: walked %d entries via the IOKit facade\n",
	    walked);
	return (0);
}
