/*
 * kextdeps — resolve a kext's dependencies and print its load order.
 *
 * The first user-visible win of the faithful OSKext port (#182, Phase 1): it
 * exercises the genuinely hard, portable-verbatim part of Apple's engine —
 * bundle discovery (OSKextCreateKextsFromURL), Info.plist parsing, and the
 * OSBundleLibraries dependency-graph topological sort (OSKextCopyLoadList) —
 * with no kld/kernel interaction at all. It is also the dependency step the
 * .ko->.kext converter (#179) needs to load kexts in the correct order.
 *
 * Usage:  kextdeps [-r repo_dir] <kext.kext> [<kext.kext> ...]
 *   -r    repository of kexts to resolve against
 *         (default /System/Library/Extensions)
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_types.h>	/* kern_return_t, before OSKext.h -> OSReturn.h */

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "OSKext.h"

static CFURLRef
url_for_path(const char *path, Boolean isDir)
{
	return CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
	    (const UInt8 *)path, (CFIndex)strlen(path), isDir);
}

static void
print_kext(OSKextRef aKext)
{
	CFStringRef ident = OSKextGetIdentifier(aKext);
	char        idbuf[256] = "<no identifier>";

	if (ident != NULL) {
		CFStringGetCString(ident, idbuf, sizeof(idbuf),
		    kCFStringEncodingUTF8);
	}
	printf("    %s\n", idbuf);
}

int
main(int argc, char *argv[])
{
	const char *repo = "/System/Library/Extensions";
	CFArrayRef  repoKexts = NULL;	/* hold repo kexts alive — see below */
	int         ch, rc = 0, i;

	while ((ch = getopt(argc, argv, "r:")) != -1) {
		switch (ch) {
		case 'r':
			repo = optarg;
			break;
		default:
			fprintf(stderr,
			    "usage: kextdeps [-r repo_dir] kext.kext ...\n");
			return (2);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		fprintf(stderr,
		    "usage: kextdeps [-r repo_dir] kext.kext ...\n");
		return (2);
	}

	/*
	 * Populate the repository so OSBundleLibraries named by the target
	 * kexts can be resolved. OSKext's registry dicts (sKextsByIdentifier,
	 * sKextsByURL) are NON-RETAINING — the kext objects are kept alive only
	 * by this array — so we must hold it until after dependency resolution
	 * below, or the dependencies vanish from the registry mid-resolve.
	 */
	if (access(repo, F_OK) == 0) {
		CFURLRef repoURL = url_for_path(repo, true);

		if (repoURL != NULL) {
			repoKexts = OSKextCreateKextsFromURL(
			    kCFAllocatorDefault, repoURL);

			if (repoKexts != NULL) {
				printf("repository %s: %ld kext(s)\n", repo,
				    (long)CFArrayGetCount(repoKexts));
			}
			CFRelease(repoURL);
		}
	} else {
		fprintf(stderr, "note: repository %s not found; dependencies "
		    "outside the named kexts may not resolve\n", repo);
	}

	for (i = 0; i < argc; i++) {
		CFURLRef  url  = url_for_path(argv[i], true);
		OSKextRef kext = (url != NULL)
		    ? OSKextCreate(kCFAllocatorDefault, url) : NULL;
		CFArrayRef loadList;

		if (url != NULL) {
			CFRelease(url);
		}
		if (kext == NULL) {
			warnx("%s: cannot open kext bundle", argv[i]);
			rc = 1;
			continue;
		}

		printf("\n%s\n", argv[i]);

		loadList = OSKextCopyLoadList(kext, /* needAllFlag */ true);
		if (loadList == NULL) {
			warnx("  unresolved dependencies (some "
			    "OSBundleLibraries not found in %s)", repo);
			rc = 1;
			CFRelease(kext);
			continue;
		}

		printf("  load order (%ld):\n",
		    (long)CFArrayGetCount(loadList));
		for (CFIndex j = 0; j < CFArrayGetCount(loadList); j++) {
			print_kext((OSKextRef)
			    CFArrayGetValueAtIndex(loadList, j));
		}

		CFRelease(loadList);
		CFRelease(kext);
	}

	if (repoKexts != NULL) {
		CFRelease(repoKexts);	/* safe to drop now resolution is done */
	}
	return (rc);
}
