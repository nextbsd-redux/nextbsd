/*
 * iokitmatchtest — libIOKit iter 2 test client. Exercises the
 * property + matching surface on the live hwregd registry: pull a
 * node's property bag as a CFDictionary, fetch a single property
 * as a CFString, look up the class name, look up nodes by class via
 * IOServiceMatching + IOServiceGetMatchingService(s), iterate the
 * matches.  Prints IOKIT-MATCH-OK on success; the boot marker
 * gates in tests/boot-test.sh.
 */
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Copy a CFString-typed dict value into `out`, "(missing)" on absence. */
static void
cfstr_value(CFDictionaryRef d, const char *key, char *out, size_t outsz)
{
	CFStringRef ck = CFStringCreateWithCString(NULL, key,
	    kCFStringEncodingUTF8);
	CFTypeRef v = ck != NULL ? CFDictionaryGetValue(d, ck) : NULL;

	if (ck != NULL)
		CFRelease(ck);
	if (v != NULL && CFGetTypeID(v) == CFStringGetTypeID() &&
	    CFStringGetCString((CFStringRef)v, out, (CFIndex)outsz,
	        kCFStringEncodingUTF8))
		return;
	(void)strlcpy(out, "(missing)", outsz);
}

int
main(void)
{
	io_registry_entry_t	root;
	CFMutableDictionaryRef	props = NULL;
	CFTypeRef		class_val;
	io_name_t		class_buf;
	io_iterator_t		it;
	io_service_t		s;
	CFMutableDictionaryRef	matching;
	char			classname[64], name[64];
	int			matched = 0;

	/* --- properties ----------------------------------------- */
	root = IORegistryGetRootEntry(kIOMainPortDefault);
	if (root == IO_OBJECT_NULL) {
		printf("IOKIT-MATCH-FAIL: IORegistryGetRootEntry\n");
		return (1);
	}
	if (IORegistryEntryCreateCFProperties(root, &props,
	    kCFAllocatorDefault, 0) != KERN_SUCCESS || props == NULL) {
		printf("IOKIT-MATCH-FAIL: IORegistryEntryCreateCFProperties\n");
		IOObjectRelease(root);
		return (1);
	}
	{
		/* hwregd's bag is always at least id/parent-id/state/name/
		 * class/driver/description/pnpinfo/location/path (10 keys);
		 * an empty bag means the unpack collapsed. */
		CFIndex n = CFDictionaryGetCount(props);

		printf("  root properties: %ld keys\n", (long)n);
		if (n < 5) {
			printf("IOKIT-MATCH-FAIL: too few root properties\n");
			return (1);
		}
		cfstr_value(props, "name", name, sizeof(name));
		cfstr_value(props, "class", classname, sizeof(classname));
		printf("  root.name=%s root.class=%s\n", name, classname);
	}
	CFRelease(props);

	/* --- IORegistryEntryCreateCFProperty (single key) -------- */
	class_val = IORegistryEntryCreateCFProperty(root, CFSTR("class"),
	    kCFAllocatorDefault, 0);
	if (class_val == NULL ||
	    CFGetTypeID(class_val) != CFStringGetTypeID()) {
		printf("IOKIT-MATCH-FAIL: CreateCFProperty(class)\n");
		return (1);
	}
	if (!CFStringGetCString((CFStringRef)class_val, classname,
	    sizeof(classname), kCFStringEncodingUTF8)) {
		printf("IOKIT-MATCH-FAIL: CFStringGetCString\n");
		return (1);
	}
	printf("  CreateCFProperty(class) -> %s\n", classname);
	CFRelease(class_val);

	/* --- IOObjectGetClass ------------------------------------ */
	if (IOObjectGetClass(root, class_buf) != KERN_SUCCESS) {
		printf("IOKIT-MATCH-FAIL: IOObjectGetClass\n");
		return (1);
	}
	printf("  IOObjectGetClass(root) -> %s\n", class_buf);

	IOObjectRelease(root);

	/* --- IOServiceMatching + IOServiceGetMatchingServices ---- */
	/* hwregd's PCI enrichment (iter 4b) reports ≥1 PCIDevice in the
	 * QEMU guest — i440FX hostb0 is always present, plus the e1000
	 * NIC the run.sh boot adds. So PCIDevice is a deterministic
	 * matching target. */
	matching = IOServiceMatching("PCIDevice");
	if (matching == NULL) {
		printf("IOKIT-MATCH-FAIL: IOServiceMatching\n");
		return (1);
	}
	if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &it)
	    != KERN_SUCCESS || it == IO_OBJECT_NULL) {
		printf("IOKIT-MATCH-FAIL: IOServiceGetMatchingServices\n");
		return (1);
	}
	/* matching has been consumed by IOServiceGetMatchingServices */
	while ((s = IOIteratorNext(it)) != IO_OBJECT_NULL) {
		io_name_t nm;

		if (IORegistryEntryGetName(s, nm) == KERN_SUCCESS && matched < 3)
			printf("  PCIDevice[%d] = %s\n", matched, nm);
		matched++;
		IOObjectRelease(s);
	}
	IOObjectRelease(it);
	if (matched == 0) {
		printf("IOKIT-MATCH-FAIL: no PCIDevice matches\n");
		return (1);
	}
	printf("  IOServiceGetMatchingServices(PCIDevice) -> %d match(es)\n",
	    matched);

	/* --- IOServiceGetMatchingService (single) ---------------- */
	s = IOServiceGetMatchingService(kIOMainPortDefault,
	    IOServiceMatching("PCIDevice"));
	if (s == IO_OBJECT_NULL) {
		printf("IOKIT-MATCH-FAIL: IOServiceGetMatchingService\n");
		return (1);
	}
	if (IORegistryEntryGetName(s, name) != KERN_SUCCESS) {
		printf("IOKIT-MATCH-FAIL: GetName on single match\n");
		IOObjectRelease(s);
		return (1);
	}
	printf("  IOServiceGetMatchingService(PCIDevice) -> %s\n", name);
	IOObjectRelease(s);

	/* --- pci-vendor as a CFNumber from PCI-enriched node ----- */
	{
		CFTypeRef vendor;

		s = IOServiceGetMatchingService(kIOMainPortDefault,
		    IOServiceMatching("PCIDevice"));
		if (s == IO_OBJECT_NULL) {
			printf("IOKIT-MATCH-FAIL: refetch PCI node\n");
			return (1);
		}
		vendor = IORegistryEntryCreateCFProperty(s,
		    CFSTR("pci-vendor"), kCFAllocatorDefault, 0);
		if (vendor != NULL && CFGetTypeID(vendor) ==
		    CFNumberGetTypeID()) {
			int64_t n = 0;

			CFNumberGetValue((CFNumberRef)vendor,
			    kCFNumberSInt64Type, &n);
			printf("  pci-vendor = 0x%04llx (CFNumber)\n",
			    (unsigned long long)n);
		} else {
			printf("  pci-vendor not present "
			    "(node missing PCI enrichment?)\n");
		}
		if (vendor != NULL)
			CFRelease(vendor);
		IOObjectRelease(s);
	}

	/* --- empty match path: a class that does not exist ------- */
	matching = IOServiceMatching("ThisClassDoesNotExist");
	if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &it)
	    != KERN_SUCCESS) {
		printf("IOKIT-MATCH-FAIL: empty-match call failed\n");
		return (1);
	}
	if (IOIteratorNext(it) != IO_OBJECT_NULL) {
		printf("IOKIT-MATCH-FAIL: empty match yielded an entry\n");
		IOObjectRelease(it);
		return (1);
	}
	IOObjectRelease(it);

	printf("IOKIT-MATCH-OK: %d PCIDevice match(es); properties + "
	    "matching work via the IOKit facade\n", matched);
	return (0);
}
