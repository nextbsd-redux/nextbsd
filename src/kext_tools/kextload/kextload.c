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
			rc = 1;
		}
		CFRelease(k);
	}

	if (repoKexts != NULL)
		CFRelease(repoKexts);
	return (rc);
}
