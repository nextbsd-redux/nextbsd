/*
 * sclocktest.c — libSystemConfiguration SCPreferences lock test client.
 *
 * Exercises SCPreferencesLock / SCPreferencesUnlock: two sessions on
 * the same preferences file contend for the exclusive lock, a session
 * cannot double-lock or unlock when it holds nothing, and
 * SCPreferencesCommitChanges takes the lock itself. run.sh runs it and
 * boot-test.sh gates on the SC-LOCK-OK / SC-LOCK-FAIL marker.
 */

#include <SystemConfiguration/SystemConfiguration.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>

#define	SCL_ID	"sclocktest.plist"

static void
fail(const char *msg)
{
	printf("SC-LOCK-FAIL: %s\n", msg);
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
	SCPreferencesRef	a	= NULL;
	SCPreferencesRef	b	= NULL;
	CFStringRef		name, prefsID, key, value;
	int			rc	= 1;

	printf("sclocktest: SCPreferences lock\n");
	fflush(stdout);

	name	= mkstr("sclocktest");
	prefsID	= mkstr(SCL_ID);
	key	= mkstr("lockkey");
	value	= mkstr("lockvalue");

	a = SCPreferencesCreate(NULL, name, prefsID);
	b = SCPreferencesCreate(NULL, name, prefsID);
	if ((a == NULL) || (b == NULL)) {
		fail("SCPreferencesCreate failed");
		goto out;
	}

	/* session A takes the lock */
	if (!SCPreferencesLock(a, TRUE)) {
		fail("SCPreferencesLock(a) failed");
		goto out;
	}
	printf("  Lock(a): acquired\n");

	/* A cannot lock twice */
	if (SCPreferencesLock(a, TRUE) || (SCError() != kSCStatusLocked)) {
		fail("double Lock(a) did not report kSCStatusLocked");
		goto out;
	}

	/* B's non-blocking lock must fail while A holds it */
	if (SCPreferencesLock(b, FALSE) || (SCError() != kSCStatusPrefsBusy)) {
		fail("Lock(b) did not report kSCStatusPrefsBusy");
		goto out;
	}
	printf("  Lock(b): correctly refused while A holds the lock\n");

	/* A releases */
	if (!SCPreferencesUnlock(a)) {
		fail("SCPreferencesUnlock(a) failed");
		goto out;
	}
	/* A cannot unlock again */
	if (SCPreferencesUnlock(a) || (SCError() != kSCStatusNeedLock)) {
		fail("double Unlock(a) did not report kSCStatusNeedLock");
		goto out;
	}

	/* now B can take it */
	if (!SCPreferencesLock(b, FALSE)) {
		fail("Lock(b) failed after A released");
		goto out;
	}
	if (!SCPreferencesUnlock(b)) {
		fail("SCPreferencesUnlock(b) failed");
		goto out;
	}
	printf("  Lock(b): acquired + released after A unlocked\n");

	/* SCPreferencesCommitChanges takes the lock itself */
	if (!SCPreferencesSetValue(a, key, value)) {
		fail("SCPreferencesSetValue failed");
		goto out;
	}
	if (!SCPreferencesCommitChanges(a)) {
		fail("SCPreferencesCommitChanges (auto-lock) failed");
		printf("  SCError: %s\n", SCErrorString(SCError()));
		goto out;
	}
	printf("  CommitChanges: auto-locked write OK\n");

	printf("SC-LOCK-OK: SCPreferences lock works\n");
	rc = 0;

    out :
	fflush(stdout);
	if (a != NULL) CFRelease(a);
	if (b != NULL) CFRelease(b);
	CFRelease(name);
	CFRelease(prefsID);
	CFRelease(key);
	CFRelease(value);
	return rc;
}
