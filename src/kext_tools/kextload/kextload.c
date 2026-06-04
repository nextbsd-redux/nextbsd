/*
 * kextload — load a .kext bundle (NextBSD proof-of-concept).
 *
 * Reads CFBundleExecutable from the bundle's Info.plist (via CFBundle) and
 * kldload(2)s Contents/MacOS/<executable> by full path. Apple's kextload
 * drives OSKext/KXKextManager; for NextBSD a .kext is packaging over the
 * unmodified FreeBSD .ko, so kld does the actual load. No codesign, no
 * dependency resolution, no IOKitPersonalities in this phase — proof-of-
 * concept trio (nextbsd#183); the faithful kext_tools/OSKext port is tracked
 * in nextbsd#182.
 *
 * We construct the executable path from the known .kext layout rather than
 * CFBundleCopyExecutableURL: swift-corelibs CFBundle on FreeBSD does not
 * resolve the macOS Contents/MacOS/ executable location, but it does parse
 * the Info.plist — so reading the key + building the path is both robust and
 * faithful to the fixed bundle layout.
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
	CFURLRef bundleURL;
	CFBundleRef bundle;
	CFStringRef execName;
	char exec[MAXPATHLEN], execPath[MAXPATHLEN];
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

	execName = CFBundleGetValueForInfoDictionaryKey(bundle,
	    CFSTR("CFBundleExecutable"));
	if (execName == NULL ||
	    CFGetTypeID(execName) != CFStringGetTypeID() ||
	    !CFStringGetCString(execName, exec, sizeof(exec),
	    kCFStringEncodingUTF8)) {
		CFRelease(bundle);
		errx(1, "%s: no CFBundleExecutable in Info.plist", bundlePath);
	}
	CFRelease(bundle);

	snprintf(execPath, sizeof(execPath), "%s/Contents/MacOS/%s",
	    bundlePath, exec);

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
