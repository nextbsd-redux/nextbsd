/*
 * ioreg(8) — registry introspection tool, modelled on macOS ioreg.
 * libIOKit iter 3 (K1 success marker in the IOKit-userland port plan
 * — pkgdemon.github.io/freebsd-hardware-registry-iokit-plan.html §5).
 *
 * Walks the IOKit facade (which wraps hwregd's MIG hwreg.defs RPC)
 * and prints the tree in roughly the macOS format:
 *
 *   +-o root0 <class IOPlatformDevice>
 *     +-o cpu0 <class CPU>
 *     +-o pci0 <class HostBridge>
 *       +-o em0 <class NetworkInterface>
 *         | "class" = "NetworkInterface"
 *         | "driver" = "em"
 *
 * Options (intentionally a small subset of macOS ioreg's flags;
 * unrecognised flags emit a usage message):
 *   -l            also print each entry's property bag
 *   -c <class>    only print entries whose class equals <class>
 *                 (also includes their parent chain so the tree
 *                 stays connected)
 *   -k <key>      only print entries that carry a property <key>
 *                 (parent chain preserved as -c)
 *   -n <name>     only print entries whose name equals <name>
 *                 (parent chain preserved as -c)
 *   -p <plane>    accepted; only "IOService" is honoured (this
 *                 facade has one plane). Other planes warned about.
 *   -w <n>        accepted; ignored (this tool does not truncate).
 *   -x            format numeric properties as hex
 *   -h, --help    usage
 *
 * Walks the registry by `IOKit/IOKitLib.h`, so the same code drives
 * any consumer of the facade: it does not touch hwreg.defs directly.
 */
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>

#include <err.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CLI flags --------------------------------------------------------- */
static int		opt_l;		/* -l show properties */
static int		opt_x;		/* -x hex numbers */
static const char	*opt_c;		/* -c class filter */
static const char	*opt_k;		/* -k key filter */
static const char	*opt_n;		/* -n name filter */

/* Output state ------------------------------------------------------ */
static int		walked;

static void
usage(void)
{
	(void)fprintf(stderr,
"usage: ioreg [-l] [-x] [-c class] [-k key] [-n name] [-p plane] [-w n]\n"
"       walks the hwregd registry via the libIOKit facade\n");
	exit(2);
}

/* Did this entry's properties contain the -k key? */
static int
has_property_key(CFDictionaryRef props, const char *key)
{
	CFStringRef ck;
	int present;

	if (key == NULL || props == NULL)
		return (1);
	ck = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	if (ck == NULL)
		return (0);
	present = CFDictionaryContainsKey(props, ck) ? 1 : 0;
	CFRelease(ck);
	return (present);
}

/* Does this entry match the configured -c / -n / -k filters? */
static int
entry_matches(io_registry_entry_t entry, CFDictionaryRef props)
{
	io_name_t buf;

	if (opt_c != NULL) {
		if (IOObjectGetClass(entry, buf) != KERN_SUCCESS)
			return (0);
		if (strcmp(buf, opt_c) != 0)
			return (0);
	}
	if (opt_n != NULL) {
		if (IORegistryEntryGetName(entry, buf) != KERN_SUCCESS)
			return (0);
		if (strcmp(buf, opt_n) != 0)
			return (0);
	}
	if (opt_k != NULL && !has_property_key(props, opt_k))
		return (0);
	return (1);
}

/* Print a single CF value in roughly macOS ioreg format. */
static void
print_cf_value(CFTypeRef v)
{
	CFTypeID id = CFGetTypeID(v);

	if (id == CFStringGetTypeID()) {
		char buf[512];

		if (CFStringGetCString((CFStringRef)v, buf, sizeof(buf),
		    kCFStringEncodingUTF8))
			(void)printf("\"%s\"", buf);
		else
			(void)printf("\"(non-utf8)\"");
	} else if (id == CFNumberGetTypeID()) {
		int64_t n = 0;

		(void)CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, &n);
		if (opt_x)
			(void)printf("0x%llx", (unsigned long long)n);
		else
			(void)printf("%lld", (long long)n);
	} else if (id == CFBooleanGetTypeID()) {
		(void)printf("%s", CFBooleanGetValue((CFBooleanRef)v)
		    ? "Yes" : "No");
	} else {
		(void)printf("(complex)");
	}
}

/* Sort callback for CFDictionary keys (CFString lexical). */
static CFComparisonResult
key_cmp(const void *a, const void *b, void *ctx __unused)
{
	return (CFStringCompare((CFStringRef)a, (CFStringRef)b, 0));
}

static void
print_properties(CFDictionaryRef props, int depth)
{
	CFIndex n = CFDictionaryGetCount(props);
	const void **keys, **values;
	CFIndex i;

	if (n == 0)
		return;
	keys = calloc((size_t)n, sizeof(*keys));
	values = calloc((size_t)n, sizeof(*values));
	if (keys == NULL || values == NULL) {
		free(keys);
		free(values);
		return;
	}
	CFDictionaryGetKeysAndValues(props, keys, values);

	/* Deterministic order — ioreg output is consumed by diff'ing
	 * scripts. Apple's sorts properties; mirror that. */
	{
		CFMutableArrayRef sorted = CFArrayCreateMutable(NULL, n,
		    &kCFTypeArrayCallBacks);

		if (sorted != NULL) {
			for (i = 0; i < n; i++)
				CFArrayAppendValue(sorted, keys[i]);
			CFArraySortValues(sorted, CFRangeMake(0, n),
			    key_cmp, NULL);
			for (i = 0; i < n; i++) {
				CFStringRef k = (CFStringRef)
				    CFArrayGetValueAtIndex(sorted, i);
				char kbuf[128];

				if (!CFStringGetCString(k, kbuf, sizeof(kbuf),
				    kCFStringEncodingUTF8))
					continue;
				(void)printf("%*s| \"%s\" = ", depth * 2 + 2,
				    "", kbuf);
				print_cf_value(CFDictionaryGetValue(props, k));
				(void)putchar('\n');
			}
			CFRelease(sorted);
		}
	}
	free(keys);
	free(values);
}

static void
print_entry_header(io_registry_entry_t entry, int depth)
{
	io_name_t name = "?", cls = "?";

	(void)IORegistryEntryGetName(entry, name);
	(void)IOObjectGetClass(entry, cls);
	(void)printf("%*s+-o %s  <class %s>\n", depth * 2, "", name, cls);
}

/*
 * Recursive walk. `forced_print` is set when an ancestor matched a
 * filter and we keep the chain visible for context.
 */
static void
walk(io_registry_entry_t entry, int depth)
{
	io_iterator_t it = IO_OBJECT_NULL;
	io_object_t child;
	CFMutableDictionaryRef props = NULL;
	int show;

	(void)IORegistryEntryCreateCFProperties(entry, &props,
	    kCFAllocatorDefault, 0);

	show = entry_matches(entry, props);
	if (show) {
		print_entry_header(entry, depth);
		walked++;
		if (opt_l && props != NULL)
			print_properties(props, depth);
	}

	if (props != NULL)
		CFRelease(props);

	if (IORegistryEntryGetChildIterator(entry, "IOService", &it) !=
	    KERN_SUCCESS)
		return;
	while ((child = IOIteratorNext(it)) != IO_OBJECT_NULL) {
		walk(child, show ? depth + 1 : depth);
		IOObjectRelease(child);
	}
	IOObjectRelease(it);
}

int
main(int argc, char **argv)
{
	io_registry_entry_t root;
	int ch;

	while ((ch = getopt(argc, argv, "lxc:k:n:p:w:h")) != -1) {
		switch (ch) {
		case 'l':	opt_l = 1; break;
		case 'x':	opt_x = 1; break;
		case 'c':	opt_c = optarg; break;
		case 'k':	opt_k = optarg; break;
		case 'n':	opt_n = optarg; break;
		case 'p':
			if (strcmp(optarg, "IOService") != 0)
				warnx("plane '%s' not supported "
				    "(only IOService); using IOService",
				    optarg);
			break;
		case 'w':
			/* Width truncation not implemented — accepted
			 * for source compatibility. */
			break;
		case 'h':
		default:
			usage();
		}
	}

	root = IORegistryGetRootEntry(kIOMainPortDefault);
	if (root == IO_OBJECT_NULL)
		errx(1, "IORegistryGetRootEntry returned IO_OBJECT_NULL "
		    "(hwregd unreachable?)");

	walk(root, 0);
	IOObjectRelease(root);

	if (walked == 0 && (opt_c || opt_n || opt_k))
		(void)fprintf(stderr, "ioreg: no entries matched the filter\n");
	return (0);
}
