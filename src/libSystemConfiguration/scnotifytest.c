/*
 * scnotifytest.c — libSystemConfiguration iter 2 change-notification
 * test client.
 *
 * Exercises the dispatch-queue notification path: one session watches
 * a key and registers an SCDynamicStoreCallBack on a dispatch queue;
 * a second session writes that key; configd must notify the watcher
 * and the callback must fire with the changed key. run.sh runs it and
 * boot-test.sh gates on the SC-NOTIFY-OK / SC-NOTIFY-FAIL marker.
 */

#include <SystemConfiguration/SCDynamicStore.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>

#include <stdio.h>
#include <string.h>

#define	SCN_KEY		"scnotifytest:watched"

static void
fail(const char *msg)
{
	printf("SC-NOTIFY-FAIL: %s\n", msg);
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
	CFStringRef		expect;		/* the key we expect to change */
	int			matched;	/* set when expect is seen */
};

/* runs on the dispatch queue when a watched key changes */
static void
notify_cb(SCDynamicStoreRef store, CFArrayRef changedKeys, void *info)
{
	struct result	*r	= info;
	CFRange		range;

	(void)store;
	range = CFRangeMake(0, CFArrayGetCount(changedKeys));
	if (CFArrayContainsValue(changedKeys, range, r->expect)) {
		r->matched = 1;
	}
	dispatch_semaphore_signal(r->sem);
}

int
main(void)
{
	SCDynamicStoreRef	watcher	= NULL;
	SCDynamicStoreRef	writer	= NULL;
	SCDynamicStoreContext	ctx;
	struct result		r;
	CFStringRef		watcherName, writerName;
	CFStringRef		key, value;
	CFArrayRef		watchKeys;
	dispatch_queue_t	queue;
	dispatch_semaphore_t	sem;
	int			rc	= 1;

	printf("scnotifytest: SCDynamicStore change notifications\n");
	fflush(stdout);

	watcherName	= mkstr("scnotifytest-watcher");
	writerName	= mkstr("scnotifytest-writer");
	key		= mkstr(SCN_KEY);
	value		= mkstr("changed");
	sem		= dispatch_semaphore_create(0);

	r.sem		= sem;
	r.expect	= key;
	r.matched	= 0;

	memset(&ctx, 0, sizeof(ctx));
	ctx.info = &r;

	/* the watcher session carries the change callback */
	watcher = SCDynamicStoreCreate(NULL, watcherName, notify_cb, &ctx);
	if (watcher == NULL) {
		fail("SCDynamicStoreCreate(watcher) failed");
		goto out;
	}
	writer = SCDynamicStoreCreate(NULL, writerName, NULL, NULL);
	if (writer == NULL) {
		fail("SCDynamicStoreCreate(writer) failed");
		goto out;
	}

	/* watch one explicit key */
	watchKeys = CFArrayCreate(NULL, (const void **)&key, 1,
				  &kCFTypeArrayCallBacks);
	if (!SCDynamicStoreSetNotificationKeys(watcher, watchKeys, NULL)) {
		fail("SCDynamicStoreSetNotificationKeys failed");
		CFRelease(watchKeys);
		goto out;
	}
	CFRelease(watchKeys);
	printf("  SetNotificationKeys: watching %s\n", SCN_KEY);

	/* deliver notifications onto a dispatch queue */
	queue = dispatch_queue_create("scnotifytest", NULL);
	if (!SCDynamicStoreSetDispatchQueue(watcher, queue)) {
		fail("SCDynamicStoreSetDispatchQueue failed");
		goto out;
	}
	printf("  SetDispatchQueue: callback scheduled\n");

	/* the writer session changes the watched key */
	if (!SCDynamicStoreSetValue(writer, key, value)) {
		fail("SCDynamicStoreSetValue(writer) failed");
		goto out;
	}
	printf("  writer set %s — waiting for the callback\n", SCN_KEY);

	/* wait up to 5s for the callback to fire */
	if (dispatch_semaphore_wait(sem,
		dispatch_time(DISPATCH_TIME_NOW,
			      (int64_t)5 * 1000 * 1000 * 1000)) != 0) {
		fail("no notification callback within 5s");
		goto out;
	}
	if (!r.matched) {
		fail("callback fired but changedKeys missed the watched key");
		goto out;
	}
	printf("  notification callback fired with the watched key\n");

	/* unschedule cleanly */
	if (!SCDynamicStoreSetDispatchQueue(watcher, NULL)) {
		fail("SCDynamicStoreSetDispatchQueue(NULL) failed");
		goto out;
	}
	printf("  SetDispatchQueue(NULL): notifications unscheduled\n");

	printf("SC-NOTIFY-OK: SCDynamicStore change notifications work\n");
	rc = 0;

    out :
	fflush(stdout);
	if (watcher != NULL) CFRelease(watcher);
	if (writer != NULL)  CFRelease(writer);
	CFRelease(watcherName);
	CFRelease(writerName);
	CFRelease(key);
	CFRelease(value);
	return rc;
}
