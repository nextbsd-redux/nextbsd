/*
 * hostnameprefset — CI fixture helper for hostnamed iter 2.
 *
 * Usage:  hostnameprefset <computer-name>
 *
 * Writes ComputerName=<computer-name> into the default
 * SCPreferences file (/Library/Preferences/SystemConfiguration/
 * preferences.plist) at path /System/System, commits, and exits.
 * The next hostnamed run will read that value and use it instead
 * of synthesizing — proving Tier 2 fires.
 *
 * Not shipped to real images (CI-only tool); lives next to
 * hostnametest under /usr/tests/freebsd-launchd-mach/.
 *
 * Issue: #86
 */

#include <SystemConfiguration/SCPreferences.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
	SCPreferencesRef prefs = NULL;
	CFStringRef session = NULL, path = NULL, key = NULL, value = NULL;
	CFMutableDictionaryRef dict = NULL;
	int rc = 1;

	if (argc != 2 || argv[1][0] == '\0') {
		(void)fprintf(stderr,
		    "usage: %s <computer-name>\n", argv[0]);
		return (2);
	}

	session = CFStringCreateWithCString(NULL, "hostnameprefset",
	    kCFStringEncodingUTF8);
	path = CFStringCreateWithCString(NULL, "/System/System",
	    kCFStringEncodingUTF8);
	key = CFStringCreateWithCString(NULL, "ComputerName",
	    kCFStringEncodingUTF8);
	value = CFStringCreateWithCString(NULL, argv[1],
	    kCFStringEncodingUTF8);
	if (session == NULL || path == NULL || key == NULL || value == NULL) {
		(void)fprintf(stderr, "CFString allocation failed\n");
		goto out;
	}

	prefs = SCPreferencesCreate(NULL, session, NULL);
	if (prefs == NULL) {
		(void)fprintf(stderr, "SCPreferencesCreate failed: %s\n",
		    SCErrorString(SCError()));
		goto out;
	}

	dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	if (dict == NULL) {
		(void)fprintf(stderr, "CFDictionaryCreateMutable failed\n");
		goto out;
	}
	CFDictionarySetValue(dict, key, value);

	if (!SCPreferencesPathSetValue(prefs, path, dict)) {
		(void)fprintf(stderr,
		    "SCPreferencesPathSetValue(%s) failed: %s\n",
		    "/System/System", SCErrorString(SCError()));
		goto out;
	}
	if (!SCPreferencesCommitChanges(prefs)) {
		(void)fprintf(stderr,
		    "SCPreferencesCommitChanges failed: %s\n",
		    SCErrorString(SCError()));
		goto out;
	}
	(void)printf("hostnameprefset: wrote ComputerName='%s' to "
	    "/Library/Preferences/SystemConfiguration/preferences.plist\n",
	    argv[1]);
	rc = 0;
out:
	if (dict != NULL) CFRelease(dict);
	if (prefs != NULL) CFRelease(prefs);
	if (value != NULL) CFRelease(value);
	if (key != NULL) CFRelease(key);
	if (path != NULL) CFRelease(path);
	if (session != NULL) CFRelease(session);
	return (rc);
}
