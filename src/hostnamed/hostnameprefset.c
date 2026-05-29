/*
 * hostnameprefset — CI fixture helper for hostnamed iters 2 / 3c.
 *
 * Usage:  hostnameprefset <computer-name>
 *         hostnameprefset --clear
 *
 * Writes ComputerName=<computer-name> into the default SCPreferences
 * file (/Library/Preferences/SystemConfiguration/preferences.plist)
 * at path /System/System, commits, and exits. The next hostnamed
 * refresh reads that value via SCPrefs and adopts it — proving the
 * SCPrefs tier fires.
 *
 * With --clear: removes /System/System from the prefs file and
 * commits. iter 3c rounds use this to reset between tiers so the
 * SCPrefs tier doesn't short-circuit DHCP / mDNS rounds. The clear
 * still flows through SCPreferencesCommitChanges, so the persistent
 * daemon's SCPrefs callback fires (a filesystem rm of preferences.
 * plist would NOT trigger the callback).
 *
 * Not shipped to real images (CI-only tool); lives next to
 * hostnametest under /usr/tests/freebsd-launchd-mach/.
 *
 * Issue: #86 (iter 2); iter 3c reshape.
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
	int clear_mode = 0;
	int rc = 1;

	if (argc != 2 || argv[1][0] == '\0') {
		(void)fprintf(stderr,
		    "usage: %s <computer-name>\n"
		    "       %s --clear\n", argv[0], argv[0]);
		return (2);
	}
	if (strcmp(argv[1], "--clear") == 0)
		clear_mode = 1;

	session = CFStringCreateWithCString(NULL, "hostnameprefset",
	    kCFStringEncodingUTF8);
	path = CFStringCreateWithCString(NULL, "/System/System",
	    kCFStringEncodingUTF8);
	key = CFStringCreateWithCString(NULL, "ComputerName",
	    kCFStringEncodingUTF8);
	if (!clear_mode)
		value = CFStringCreateWithCString(NULL, argv[1],
		    kCFStringEncodingUTF8);
	if (session == NULL || path == NULL || key == NULL ||
	    (!clear_mode && value == NULL)) {
		(void)fprintf(stderr, "CFString allocation failed\n");
		goto out;
	}

	prefs = SCPreferencesCreate(NULL, session, NULL);
	if (prefs == NULL) {
		(void)fprintf(stderr, "SCPreferencesCreate failed: %s\n",
		    SCErrorString(SCError()));
		goto out;
	}

	if (clear_mode) {
		/* Idempotent — succeeds even if /System/System is absent;
		 * commit still fires the callback. */
		(void)SCPreferencesPathRemoveValue(prefs, path);
		if (!SCPreferencesCommitChanges(prefs)) {
			(void)fprintf(stderr,
			    "SCPreferencesCommitChanges(--clear) failed: %s\n",
			    SCErrorString(SCError()));
			goto out;
		}
		(void)printf("hostnameprefset: cleared /System/System "
		    "(ComputerName removed, commit fired)\n");
		rc = 0;
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
