/*
 * scprefsnotifytest.c — libSystemConfiguration SCPreferences change-
 * notification test client.
 *
 * Exercises SCPreferencesSetCallback + SCPreferencesSetDispatchQueue:
 * one session watches a preferences file on a dispatch queue, another
 * commits a change to it, and the watcher's callback must fire. This
 * crosses SCPreferences -> SCDynamicStore -> configd and back. run.sh
 * runs it and boot-test.sh gates on the SC-PNOTIFY-OK / SC-PNOTIFY-FAIL
 * marker.
 */

#include <SystemConfiguration/SystemConfiguration.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>

#include <stdio.h>
#include <string.h>

#define	SCPN_ID		"scprefsnotifytest.plist"

static void
fail(const char *msg)
{
	printf("SC-PNOTIFY-FAIL: %s\n", msg);
	fflush(stdout);
}

static CFStringRef
mkstr(const char *s)
{
	return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
}

/* shared between main and the notification callback */
struct result {
	dispatch_semaphore_t	sem;
	int			committed;	/* commit notification seen */
};

/* runs on the dispatch queue when the watched preferences change */
static void
prefs_cb(SCPreferencesRef prefs, SCPreferencesNotification type, void *info)
{
	struct result	*r	= info;

	(void)prefs;
	if ((type & kSCPreferencesNotificationCommit) != 0) {
		r->committed = 1;
	}
	dispatch_semaphore_signal(r->sem);
}

int
main(void)
{
	SCPreferencesRef	watcher	= NULL;
	SCPreferencesRef	writer	= NULL;
	SCPreferencesContext	ctx;
	struct result		r;
	CFStringRef		watcherName, writerName, prefsID;
	CFStringRef		key, value;
	dispatch_queue_t	queue;
	dispatch_semaphore_t	sem;
	int			rc	= 1;

	printf("scprefsnotifytest: SCPreferences change notifications\n");
	fflush(stdout);

	watcherName	= mkstr("scprefsnotifytest-watcher");
	writerName	= mkstr("scprefsnotifytest-writer");
	prefsID		= mkstr(SCPN_ID);
	key		= mkstr("notifykey");
	value		= mkstr("notifyvalue");
	sem		= dispatch_semaphore_create(0);

	r.sem		= sem;
	r.committed	= 0;

	memset(&ctx, 0, sizeof(ctx));
	ctx.info = &r;

	watcher = SCPreferencesCreate(NULL, watcherName, prefsID);
	writer  = SCPreferencesCreate(NULL, writerName, prefsID);
	if ((watcher == NULL) || (writer == NULL)) {
		fail("SCPreferencesCreate failed");
		goto out;
	}

	/* the watcher takes a callback on a dispatch queue */
	if (!SCPreferencesSetCallback(watcher, prefs_cb, &ctx)) {
		fail("SCPreferencesSetCallback failed");
		goto out;
	}
	queue = dispatch_queue_create("scprefsnotifytest", NULL);
	if (!SCPreferencesSetDispatchQueue(watcher, queue)) {
		fail("SCPreferencesSetDispatchQueue failed");
		goto out;
	}
	printf("  watcher: callback scheduled\n");

	/* the writer commits a change */
	if (!SCPreferencesSetValue(writer, key, value) ||
	    !SCPreferencesCommitChanges(writer)) {
		fail("writer SetValue/CommitChanges failed");
		goto out;
	}
	printf("  writer: committed a change — waiting for the callback\n");

	/* wait up to 5s for the watcher's callback */
	if (dispatch_semaphore_wait(sem,
		dispatch_time(DISPATCH_TIME_NOW,
			      (int64_t)5 * 1000 * 1000 * 1000)) != 0) {
		fail("no SCPreferences callback within 5s");
		goto out;
	}
	if (!r.committed) {
		fail("callback fired without kSCPreferencesNotificationCommit");
		goto out;
	}
	printf("  watcher: commit notification delivered\n");

	(void) SCPreferencesSetDispatchQueue(watcher, NULL);

	printf("SC-PNOTIFY-OK: SCPreferences change notifications work\n");
	rc = 0;

    out :
	fflush(stdout);
	if (watcher != NULL) CFRelease(watcher);
	if (writer != NULL)  CFRelease(writer);
	CFRelease(watcherName);
	CFRelease(writerName);
	CFRelease(prefsID);
	CFRelease(key);
	CFRelease(value);
	return rc;
}
