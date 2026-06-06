/*
 * kextd — push kext IOKitPersonalities into the in-kernel IOCatalogue (U1,
 * nextbsd#217).
 *
 * The Apple-faithful loader half of the in-kernel IOKit matcher (umbrella
 * #211, Option B, mechanism (a)): userland parses each kext bundle's
 * IOKitPersonalities and PUSHES them to the kernel; the kernel (K2 #215 store,
 * K3 #216 matcher) owns matching. This is the analog of Apple kextd's boot-time
 * IOCatalogueSendData.
 *
 * What it does now (push): open every kext under a repository (default
 * /System/Library/Extensions) with the OSKext engine, take each personality's
 * device-id match table (IOProviderClass / IOProbeScore / IOPCIPrimaryMatch),
 * and submit one flat record per personality to /dev/iocatalogue via
 * IOCATIOCADD. It first IOCATIOCFLUSHes, so running it again simply re-pushes
 * the current set (idempotent). The kernel never parses XML — kextd does.
 *
 * Invoked on demand for now (e.g. by the on-image IOKit test). The launchd
 * boot-time auto-push is deferred to K3 (#216): a RunAtLoad CF/OSKext job this
 * early wedges launchd's boot dispatch, so kextd grows its boot integration
 * once it is a persistent daemon whose ordering is designed.
 *
 * Not yet (lands with K3, #216): listening on /dev/devctl for the kernel's
 * "IOKIT load <bundle-id>" requests and kextload-ing the named bundle. For now
 * kextd populates the catalogue and exits; the matcher that consumes it is K3.
 *
 * Usage:  kextd [-r repo_dir] [-v]
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_types.h>	/* kern_return_t, before OSKext.h -> OSReturn.h */

#include <err.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "OSKext.h"
#include "iocatalogue.h"	/* vendored ABI; canonical copy in nextbsd-kernel */

static bool verbose;

static void
tool_log(OSKextRef aKext __unused, OSKextLogSpec spec __unused,
    const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

static CFURLRef
url_for_path(const char *path)
{
	return CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
	    (const UInt8 *)path, (CFIndex)strlen(path), true);
}

/* Copy a CFString value into a C buffer; returns false if absent/not a string. */
static bool
dict_string(CFDictionaryRef d, CFStringRef key, char *buf, size_t buflen)
{
	CFStringRef s = CFDictionaryGetValue(d, key);

	if (s == NULL || CFGetTypeID(s) != CFStringGetTypeID())
		return (false);
	return (CFStringGetCString(s, buf, (CFIndex)buflen,
	    kCFStringEncodingUTF8) != false);
}

/* Fetch an integer CFNumber value; returns def if absent/not a number. */
static int32_t
dict_int(CFDictionaryRef d, CFStringRef key, int32_t def)
{
	CFNumberRef n = CFDictionaryGetValue(d, key);
	int32_t v;

	if (n == NULL || CFGetTypeID(n) != CFNumberGetTypeID())
		return (def);
	if (!CFNumberGetValue(n, kCFNumberSInt32Type, &v))
		return (def);
	return (v);
}

/*
 * Parse an IOPCIPrimaryMatch string ("0x24f38086 0x...") into a freshly
 * malloc'd array of match words. Returns the count (0 on none/absent); *out is
 * set to the array (caller frees) or NULL.
 */
static uint32_t
parse_match(CFDictionaryRef d, uint32_t **out)
{
	char buf[8192];
	uint32_t *words;
	uint32_t n = 0;
	char *p, *tok, *save;

	*out = NULL;
	if (!dict_string(d, CFSTR("IOPCIPrimaryMatch"), buf, sizeof(buf)))
		return (0);

	words = calloc(IOCAT_MAX_MATCH, sizeof(*words));
	if (words == NULL)
		return (0);
	for (p = buf; (tok = strtok_r(p, " ,\t", &save)) != NULL; p = NULL) {
		if (n >= IOCAT_MAX_MATCH)
			break;
		words[n++] = (uint32_t)strtoul(tok, NULL, 16);
	}
	if (n == 0) {
		free(words);
		return (0);
	}
	*out = words;
	return (n);
}

/* Push one personality dict; returns 1 if pushed, 0 if skipped, -1 on error. */
static int
push_personality(int fd, CFDictionaryRef p)
{
	struct iocat_add add;
	char provider[64];
	uint32_t *words;
	uint32_t n;
	int rc;

	memset(&add, 0, sizeof(add));
	if (!dict_string(p, CFSTR("CFBundleIdentifier"), add.bundle_id,
	    sizeof(add.bundle_id)))
		return (0);	/* no bundle id -> can't be loaded; skip */

	if (dict_string(p, CFSTR("IOProviderClass"), provider, sizeof(provider)) &&
	    strcmp(provider, "IOPCIDevice") == 0)
		add.provider_class = IOCAT_PROVIDER_IOPCIDEVICE;
	else
		add.provider_class = IOCAT_PROVIDER_UNKNOWN;

	n = parse_match(p, &words);
	if (n == 0)
		return (0);	/* no device-id match table -> nothing to match */

	add.probe_score = dict_int(p, CFSTR("IOProbeScore"), 0);
	add.nmatch = n;
	add.match = (uint64_t)(uintptr_t)words;

	rc = ioctl(fd, IOCATIOCADD, &add);
	free(words);
	if (rc != 0) {
		warn("IOCATIOCADD %s", add.bundle_id);
		return (-1);
	}
	if (verbose)
		printf("kextd: pushed %s (%s, score %d, %u ids)\n",
		    add.bundle_id,
		    add.provider_class == IOCAT_PROVIDER_IOPCIDEVICE ?
		    "IOPCIDevice" : "?", add.probe_score, n);
	return (1);
}

int
main(int argc, char *argv[])
{
	const char *repo = "/System/Library/Extensions";
	CFArrayRef repoKexts = NULL;	/* non-retaining registry: hold alive */
	CFArrayRef personalities = NULL;
	CFURLRef u;
	int fd, ch;
	int pushed = 0, skipped = 0, failed = 0;
	CFIndex i, count;

	while ((ch = getopt(argc, argv, "r:v")) != -1) {
		switch (ch) {
		case 'r':
			repo = optarg;
			break;
		case 'v':
			verbose = true;
			break;
		default:
			errx(2, "usage: kextd [-r repo_dir] [-v]");
		}
	}

	OSKextSetLogOutputFunction(&tool_log);
	if (verbose)
		OSKextSetLogFilter(kOSKextLogDetailLevel |
		    kOSKextLogVerboseFlagsMask, false);

	fd = open("/dev/iocatalogue", O_RDWR);
	if (fd < 0)
		err(1, "open /dev/iocatalogue (is the K2 kernel loaded?)");

	/* Idempotent: drop the prior set, then re-push the current repo. */
	if (ioctl(fd, IOCATIOCFLUSH) != 0)
		warn("IOCATIOCFLUSH");

	u = url_for_path(repo);
	if (u == NULL)
		errx(1, "%s: cannot form URL", repo);
	repoKexts = OSKextCreateKextsFromURL(kCFAllocatorDefault, u);
	CFRelease(u);
	if (repoKexts == NULL)
		errx(1, "%s: no kexts found", repo);

	/* All personalities across all open kexts, processed for the kernel. */
	personalities = OSKextCopyPersonalitiesOfKexts(NULL);
	if (personalities == NULL)
		errx(1, "no personalities in %s", repo);

	count = CFArrayGetCount(personalities);
	for (i = 0; i < count; i++) {
		CFDictionaryRef p = CFArrayGetValueAtIndex(personalities, i);
		int r;

		if (p == NULL || CFGetTypeID(p) != CFDictionaryGetTypeID())
			continue;
		r = push_personality(fd, p);
		if (r > 0)
			pushed++;
		else if (r == 0)
			skipped++;
		else
			failed++;
	}

	printf("kextd: %d personalities pushed, %d skipped, %d failed (repo %s)\n",
	    pushed, skipped, failed, repo);

	CFRelease(personalities);
	CFRelease(repoKexts);
	close(fd);
	return (failed > 0 ? 1 : 0);
}
