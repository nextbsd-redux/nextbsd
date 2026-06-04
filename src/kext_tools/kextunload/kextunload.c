/*
 * kextunload — unload a kext (NextBSD #182/#196).
 *
 * Graduated from the minimal #183 kld unloader to Apple's OSKext engine:
 * OSKextUnload maps the kext bundle to its loaded kld module (by
 * CFBundleExecutable) and kldunload(2)s it; OSKextUnloadKextWithIdentifier
 * does the same given a CFBundleIdentifier. Unload is idempotent (a kext that
 * isn't loaded is treated as success).
 *
 * Usage:  kextunload <bundle.kext> ...
 *         kextunload -b <CFBundleIdentifier> ...
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_types.h>	/* kern_return_t, before OSKext.h -> OSReturn.h */

#include <err.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "OSKext.h"

int
main(int argc, char *argv[])
{
	Boolean byIdentifier = false;
	int     ch, rc = 0, i;

	while ((ch = getopt(argc, argv, "b")) != -1) {
		switch (ch) {
		case 'b':
			byIdentifier = true;
			break;
		default:
			errx(2, "usage: kextunload [-b] "
			    "<bundle.kext | identifier> ...");
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1)
		errx(2, "usage: kextunload [-b] "
		    "<bundle.kext | identifier> ...");

	for (i = 0; i < argc; i++) {
		OSReturn r;

		if (byIdentifier) {
			CFStringRef id = CFStringCreateWithCString(
			    kCFAllocatorDefault, argv[i], kCFStringEncodingUTF8);

			if (id == NULL) {
				warnx("%s: bad identifier", argv[i]);
				rc = 1;
				continue;
			}
			r = OSKextUnloadKextWithIdentifier(id,
			    /* terminateAndRemovePersonalities */ true);
			CFRelease(id);
		} else {
			CFURLRef  u = CFURLCreateFromFileSystemRepresentation(
			    kCFAllocatorDefault, (const UInt8 *)argv[i],
			    (CFIndex)strlen(argv[i]), true);
			OSKextRef k = (u != NULL)
			    ? OSKextCreate(kCFAllocatorDefault, u) : NULL;

			if (u != NULL)
				CFRelease(u);
			if (k == NULL) {
				warnx("%s: cannot open kext bundle", argv[i]);
				rc = 1;
				continue;
			}
			r = OSKextUnload(k,
			    /* terminateAndRemovePersonalities */ true);
			CFRelease(k);
		}

		if (r == kOSReturnSuccess) {
			printf("kextunload: unloaded %s\n", argv[i]);
		} else {
			warnx("%s: unload failed (OSReturn 0x%x)", argv[i],
			    (unsigned)r);
			rc = 1;
		}
	}

	return (rc);
}
