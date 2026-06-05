/*
 * kextload — load a kext and its dependencies, in order (NextBSD #182/#196).
 *
 * Graduated from the minimal #183 kld loader to Apple's OSKext engine: it
 * resolves the kext's OSBundleLibraries dependency graph and kldload(2)s the
 * dependency-ordered load list (OSKextLoad). A repository (default
 * /System/Library/Extensions) supplies the libraries the kext names. kld
 * performs each load; there is no codesign and personalities are kextd's job
 * (#177). For a self-contained kext with no dependencies this is equivalent to
 * the old single kldload, but it now loads libraries first, correctly.
 *
 * Usage:  kextload [-r repo_dir] <bundle.kext> [<bundle.kext> ...]
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_types.h>	/* kern_return_t, before OSKext.h -> OSReturn.h */

#include <err.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "OSKext.h"

static CFURLRef
url_for_path(const char *path)
{
	return CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
	    (const UInt8 *)path, (CFIndex)strlen(path), true);
}

/*
 * Route OSKext library log/diagnostic output to stderr so a failed load
 * explains itself (which property/executable check failed) instead of just
 * an opaque OSReturn. Mirrors what Apple's kextload does.
 */
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

int
main(int argc, char *argv[])
{
	const char *repo = "/System/Library/Extensions";
	CFArrayRef  repoKexts = NULL;	/* non-retaining registry: hold alive */
	int         ch, rc = 0, i;

	while ((ch = getopt(argc, argv, "r:")) != -1) {
		switch (ch) {
		case 'r':
			repo = optarg;
			break;
		default:
			errx(2, "usage: kextload [-r repo_dir] bundle.kext ...");
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1)
		errx(2, "usage: kextload [-r repo_dir] bundle.kext ...");

	/*
	 * Record diagnostics and route library messages to stderr, so a
	 * validation/dependency failure prints the specific reason.
	 */
	OSKextSetLogOutputFunction(&tool_log);
	OSKextSetLogFilter(kOSKextLogDetailLevel | kOSKextLogVerboseFlagsMask,
	    /* kernelFlag */ false);
	OSKextSetRecordsDiagnostics(kOSKextDiagnosticsFlagAll);

	/*
	 * Populate the repository so OSBundleLibraries resolve. OSKext's
	 * registry is non-retaining, so the array must outlive the loads.
	 */
	if (access(repo, F_OK) == 0) {
		CFURLRef u = url_for_path(repo);

		if (u != NULL) {
			repoKexts = OSKextCreateKextsFromURL(kCFAllocatorDefault, u);
			CFRelease(u);
		}
	}

	for (i = 0; i < argc; i++) {
		CFURLRef  u = url_for_path(argv[i]);
		OSKextRef k = (u != NULL)
		    ? OSKextCreate(kCFAllocatorDefault, u) : NULL;
		OSReturn  r;

		if (u != NULL)
			CFRelease(u);
		if (k == NULL) {
			warnx("%s: cannot open kext bundle", argv[i]);
			rc = 1;
			continue;
		}

		r = OSKextLoad(k);
		if (r == kOSReturnSuccess) {
			printf("kextload: loaded %s (dependency-ordered)\n",
			    argv[i]);
		} else {
			warnx("%s: load failed (OSReturn 0x%x)", argv[i],
			    (unsigned)r);
			/* Print the specific validation/dependency problems. */
			OSKextLogDiagnostics(k, kOSKextDiagnosticsFlagAll);
			rc = 1;
		}
		CFRelease(k);
	}

	if (repoKexts != NULL)
		CFRelease(repoKexts);
	return (rc);
}
