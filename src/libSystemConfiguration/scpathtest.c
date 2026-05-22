/*
 * scpathtest.c — libSystemConfiguration SCPreferences iter 2 path-
 * accessor test client.
 *
 * Exercises the '/'-separated path accessors: set a dictionary at a
 * nested path (creating the intermediate dictionaries), read it back
 * (and read an intermediate level), commit, re-open and confirm it
 * persisted, then remove it. run.sh runs it and boot-test.sh gates on
 * the SC-PATH-OK / SC-PATH-FAIL marker.
 */

#include <SystemConfiguration/SystemConfiguration.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>

#define	SCT_ID		"scpathtest.plist"
#define	SCT_PATH	"/net/service"
#define	SCT_PARENT	"/net"

static void
fail(const char *msg)
{
	printf("SC-PATH-FAIL: %s\n", msg);
	fflush(stdout);
}

static CFStringRef
mkstr(const char *s)
{
	return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
}

int
main(void)
{
	SCPreferencesRef	prefs	= NULL;
	CFStringRef		name, prefsID;
	CFStringRef		path, parent, leafKey, leafValue, childKey;
	CFMutableDictionaryRef	entity	= NULL;
	CFDictionaryRef		got;
	int			rc	= 1;

	printf("scpathtest: SCPreferences path accessors\n");
	fflush(stdout);

	name		= mkstr("scpathtest");
	prefsID		= mkstr(SCT_ID);
	path		= mkstr(SCT_PATH);
	parent		= mkstr(SCT_PARENT);
	leafKey		= mkstr("leaf");
	leafValue	= mkstr("leaf-value");
	childKey	= mkstr("service");

	entity = CFDictionaryCreateMutable(NULL, 0,
					   &kCFTypeDictionaryKeyCallBacks,
					   &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(entity, leafKey, leafValue);

	/* set a dictionary at a nested path */
	prefs = SCPreferencesCreate(NULL, name, prefsID);
	if (prefs == NULL) {
		fail("SCPreferencesCreate failed");
		goto out;
	}
	if (!SCPreferencesPathSetValue(prefs, path, entity)) {
		fail("SCPreferencesPathSetValue failed");
		goto out;
	}
	printf("  PathSetValue: %s created\n", SCT_PATH);

	/* read it back */
	got = SCPreferencesPathGetValue(prefs, path);
	if (got == NULL) {
		fail("SCPreferencesPathGetValue returned NULL");
		goto out;
	}
	{
		CFTypeRef	v = CFDictionaryGetValue(got, leafKey);

		if ((v == NULL) || !CFEqual(v, leafValue)) {
			fail("path entity did not round-trip");
			goto out;
		}
	}

	/* the intermediate level must hold the child */
	got = SCPreferencesPathGetValue(prefs, parent);
	if ((got == NULL) ||
	    (CFDictionaryGetValue(got, childKey) == NULL)) {
		fail("intermediate path missing the child dictionary");
		goto out;
	}
	printf("  PathGetValue: nested + intermediate read OK\n");

	/* commit, re-open, confirm it persisted */
	if (!SCPreferencesCommitChanges(prefs)) {
		fail("SCPreferencesCommitChanges failed");
		goto out;
	}
	CFRelease(prefs);
	prefs = SCPreferencesCreate(NULL, name, prefsID);
	if (prefs == NULL) {
		fail("SCPreferencesCreate (reopen) failed");
		goto out;
	}
	got = SCPreferencesPathGetValue(prefs, path);
	if ((got == NULL) ||
	    !CFEqual(CFDictionaryGetValue(got, leafKey), leafValue)) {
		fail("path entity did not persist");
		goto out;
	}
	printf("  CommitChanges/reopen: path persisted\n");

	/* remove it */
	if (!SCPreferencesPathRemoveValue(prefs, path)) {
		fail("SCPreferencesPathRemoveValue failed");
		goto out;
	}
	if (SCPreferencesPathGetValue(prefs, path) != NULL) {
		fail("path still present after remove");
		goto out;
	}
	printf("  PathRemoveValue: path removed\n");

	printf("SC-PATH-OK: SCPreferences path accessors work\n");
	rc = 0;

    out :
	fflush(stdout);
	if (prefs != NULL) CFRelease(prefs);
	if (entity != NULL) CFRelease(entity);
	CFRelease(name);
	CFRelease(prefsID);
	CFRelease(path);
	CFRelease(parent);
	CFRelease(leafKey);
	CFRelease(leafValue);
	CFRelease(childKey);
	return rc;
}
