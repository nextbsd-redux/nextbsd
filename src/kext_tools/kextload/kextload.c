/*
 * kextload — load a .kext bundle (NextBSD proof-of-concept).
 *
 * Opens the bundle with CFBundle, resolves Contents/MacOS/<executable>,
 * and kldload(2)s it. Apple's kextload drives OSKext/KXKextManager; for
 * NextBSD a .kext is packaging over the unmodified FreeBSD .ko, so kld
 * does the actual load. No codesign, no dependency resolution, no
 * IOKitPersonalities in this phase — proof-of-concept trio (nextbsd#183);
 * the faithful kext_tools/OSKext port is tracked in nextbsd#182.
 */
#include <CoreFoundation/CoreFoundation.h>

#include <sys/param.h>
#include <sys/linker.h>

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int
main(int argc, char **argv)
{
	const char *bundlePath;
	CFURLRef bundleURL, execURL;
	CFBundleRef bundle;
	char execPath[MAXPATHLEN];
	int fileid;

	if (argc != 2)
		errx(1, "usage: kextload <bundle.kext>");
	bundlePath = argv[1];

	bundleURL = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
	    (const UInt8 *)bundlePath, (CFIndex)strlen(bundlePath), true);
	if (bundleURL == NULL)
		errx(1, "cannot make URL for %s", bundlePath);

	bundle = CFBundleCreate(kCFAllocatorDefault, bundleURL);
	CFRelease(bundleURL);
	if (bundle == NULL)
		errx(1, "%s: not a bundle", bundlePath);

	execURL = CFBundleCopyExecutableURL(bundle);
	if (execURL == NULL) {
		CFRelease(bundle);
		errx(1, "%s: no executable (check CFBundleExecutable)", bundlePath);
	}
	if (!CFURLGetFileSystemRepresentation(execURL, true,
	    (UInt8 *)execPath, sizeof(execPath))) {
		CFRelease(execURL);
		CFRelease(bundle);
		errx(1, "%s: cannot resolve executable path", bundlePath);
	}
	CFRelease(execURL);
	CFRelease(bundle);

	/* A full path bypasses kern.module_path; the .ko loads by content. */
	fileid = kldload(execPath);
	if (fileid < 0) {
		if (errno == EEXIST) {
			printf("kextload: %s already loaded\n", bundlePath);
			return (0);
		}
		err(1, "kldload(%s)", execPath);
	}
	printf("kextload: loaded %s (id %d)\n", bundlePath, fileid);
	return (0);
}
