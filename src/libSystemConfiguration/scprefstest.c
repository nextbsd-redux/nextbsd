/*
 * scprefstest.c — libSystemConfiguration SCPreferences iter 1 test
 * client.
 *
 * Exercises the SCPreferences read / edit / commit cycle: open a
 * preferences session, set values, commit, then re-open and confirm
 * the values persisted; list keys; remove a key, commit, and confirm
 * the removal persisted. run.sh runs it and boot-test.sh gates on the
 * SC-PREFS-OK / SC-PREFS-FAIL marker.
 */

#include <SystemConfiguration/SystemConfiguration.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>

#define	SCP_ID		"scprefstest.plist"
#define	SCP_KEY1	"key1"
#define	SCP_KEY2	"key2"

static void
fail(const char *msg)
{
	printf("SC-PREFS-FAIL: %s\n", msg);
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
	CFStringRef		key1, key2, value1;
	CFMutableDictionaryRef	value2	= NULL;
	CFPropertyListRef	got;
	CFArrayRef		keys;
	int			rc	= 1;

	printf("scprefstest: SCPreferences read/edit/commit\n");
	fflush(stdout);

	name	= mkstr("scprefstest");
	prefsID	= mkstr(SCP_ID);
	key1	= mkstr(SCP_KEY1);
	key2	= mkstr(SCP_KEY2);
	value1	= mkstr("value-1");

	value2 = CFDictionaryCreateMutable(NULL, 0,
					   &kCFTypeDictionaryKeyCallBacks,
					   &kCFTypeDictionaryValueCallBacks);
	{
		CFStringRef	dk = mkstr("inner");
		CFStringRef	dv = mkstr("inner-value");
		CFDictionarySetValue(value2, dk, dv);
		CFRelease(dk);
		CFRelease(dv);
	}

	/* open a session, set two values, commit */
	prefs = SCPreferencesCreate(NULL, name, prefsID);
	if (prefs == NULL) {
		fail("SCPreferencesCreate failed");
		goto out;
	}
	if (!SCPreferencesSetValue(prefs, key1, value1) ||
	    !SCPreferencesSetValue(prefs, key2, value2)) {
		fail("SCPreferencesSetValue failed");
		goto out;
	}
	if (!SCPreferencesCommitChanges(prefs)) {
		fail("SCPreferencesCommitChanges failed");
		printf("  SCError: %s\n", SCErrorString(SCError()));
		goto out;
	}
	CFRelease(prefs);
	prefs = NULL;
	printf("  Create/SetValue/CommitChanges: 2 keys written\n");

	/* re-open and confirm the values persisted */
	prefs = SCPreferencesCreate(NULL, name, prefsID);
	if (prefs == NULL) {
		fail("SCPreferencesCreate (reopen) failed");
		goto out;
	}
	got = SCPreferencesGetValue(prefs, key1);
	if ((got == NULL) || !CFEqual(got, value1)) {
		fail("key1 did not persist");
		goto out;
	}
	got = SCPreferencesGetValue(prefs, key2);
	if ((got == NULL) || !CFEqual(got, value2)) {
		fail("key2 (dictionary) did not persist");
		goto out;
	}
	printf("  reopen/GetValue: both values persisted\n");

	/* CopyKeyList must list both keys */
	keys = SCPreferencesCopyKeyList(prefs);
	if (keys == NULL) {
		fail("SCPreferencesCopyKeyList returned NULL");
		goto out;
	}
	{
		CFRange	range = CFRangeMake(0, CFArrayGetCount(keys));

		if (!CFArrayContainsValue(keys, range, key1) ||
		    !CFArrayContainsValue(keys, range, key2)) {
			fail("CopyKeyList missing a key");
			CFRelease(keys);
			goto out;
		}
	}
	CFRelease(keys);
	printf("  CopyKeyList: both keys listed\n");

	/* remove key1, commit */
	if (!SCPreferencesRemoveValue(prefs, key1)) {
		fail("SCPreferencesRemoveValue failed");
		goto out;
	}
	if (!SCPreferencesCommitChanges(prefs)) {
		fail("SCPreferencesCommitChanges (after remove) failed");
		goto out;
	}
	CFRelease(prefs);
	prefs = NULL;

	/* re-open: key1 gone, key2 still there */
	prefs = SCPreferencesCreate(NULL, name, prefsID);
	if (prefs == NULL) {
		fail("SCPreferencesCreate (reopen 2) failed");
		goto out;
	}
	if (SCPreferencesGetValue(prefs, key1) != NULL) {
		fail("key1 still present after remove + commit");
		goto out;
	}
	if (SCError() != kSCStatusNoKey) {
		fail("GetValue of removed key did not report kSCStatusNoKey");
		goto out;
	}
	if (SCPreferencesGetValue(prefs, key2) == NULL) {
		fail("key2 lost after removing key1");
		goto out;
	}
	printf("  RemoveValue: removal persisted, other key kept\n");

	printf("SC-PREFS-OK: SCPreferences read/edit/commit works\n");
	rc = 0;

    out :
	fflush(stdout);
	if (prefs != NULL) CFRelease(prefs);
	CFRelease(name);
	CFRelease(prefsID);
	CFRelease(key1);
	CFRelease(key2);
	CFRelease(value1);
	CFRelease(value2);
	return rc;
}
