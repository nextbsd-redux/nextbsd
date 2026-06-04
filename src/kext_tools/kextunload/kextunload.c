/*
 * kextunload — unload a .kext bundle's module (NextBSD proof-of-concept).
 *
 * Reads CFBundleExecutable from the bundle's Info.plist (the .ko registers
 * under that name when kextload kldload's Contents/MacOS/<executable>), finds
 * the matching loaded kld file, and kldunload(2)s it. kld-backed; proof-of-
 * concept trio (nextbsd#183), faithful kext_tools/OSKext port tracked in
 * nextbsd#182.
 */
#include <CoreFoundation/CoreFoundation.h>

#include <sys/param.h>
#include <sys/linker.h>

#include <err.h>
#include <libgen.h>
#include <stdio.h>
#include <string.h>

int
main(int argc, char **argv)
{
	const char *bundlePath;
	CFURLRef bundleURL;
	CFBundleRef bundle;
	CFStringRef execName;
	char want[MAXPATHLEN];
	int fileid;

	if (argc != 2)
		errx(1, "usage: kextunload <bundle.kext>");
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
	    !CFStringGetCString(execName, want, sizeof(want),
	    kCFStringEncodingUTF8)) {
		CFRelease(bundle);
		errx(1, "%s: no CFBundleExecutable in Info.plist", bundlePath);
	}
	CFRelease(bundle);

	/* The kld file registers under the executable's basename. */
	for (fileid = kldnext(0); fileid > 0; fileid = kldnext(fileid)) {
		struct kld_file_stat stat;
		char name[MAXPATHLEN];

		stat.version = sizeof(stat);
		if (kldstat(fileid, &stat) < 0)
			continue;
		strlcpy(name, stat.name, sizeof(name));
		if (strcmp(basename(name), want) == 0) {
			if (kldunload(fileid) < 0)
				err(1, "kldunload(%s)", want);
			printf("kextunload: unloaded %s (was id %d)\n",
			    bundlePath, fileid);
			return (0);
		}
	}
	errx(1, "kextunload: %s not loaded", bundlePath);
}
