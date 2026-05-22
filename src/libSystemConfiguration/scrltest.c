/*
 * scrltest.c — libSystemConfiguration iter 3 run-loop-source test
 * client.
 *
 * Exercises the run-loop notification path: one session watches a key
 * and adds an SCDynamicStoreCreateRunLoopSource() to the current run
 * loop; a second session writes that key; running the run loop must
 * deliver the SCDynamicStoreCallBack with the changed key. run.sh runs
 * it and boot-test.sh gates on the SC-RUNLOOP-OK / SC-RUNLOOP-FAIL
 * marker.
 */

#include <SystemConfiguration/SCDynamicStore.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>
#include <string.h>

#define	SCR_KEY		"scrltest:watched"

static void
fail(const char *msg)
{
	printf("SC-RUNLOOP-FAIL: %s\n", msg);
	fflush(stdout);
}

static CFStringRef
mkstr(const char *s)
{
	return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
}

/* shared between main and the notification callback */
struct result {
	CFStringRef	expect;		/* the key we expect to change */
	int		matched;	/* set when expect is seen */
};

/* runs on the run loop thread when a watched key changes */
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
	CFRunLoopSourceRef	rls	= NULL;
	int			rc	= 1;

	printf("scrltest: SCDynamicStore run-loop-source notifications\n");
	fflush(stdout);

	watcherName	= mkstr("scrltest-watcher");
	writerName	= mkstr("scrltest-writer");
	key		= mkstr(SCR_KEY);
	value		= mkstr("changed");

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
	printf("  SetNotificationKeys: watching %s\n", SCR_KEY);

	/* create a run loop source and add it to this thread's run loop */
	rls = SCDynamicStoreCreateRunLoopSource(NULL, watcher, 0);
	if (rls == NULL) {
		fail("SCDynamicStoreCreateRunLoopSource failed");
		goto out;
	}
	CFRunLoopAddSource(CFRunLoopGetCurrent(), rls, kCFRunLoopDefaultMode);
	printf("  CreateRunLoopSource: source scheduled\n");

	/* the writer session changes the watched key */
	if (!SCDynamicStoreSetValue(writer, key, value)) {
		fail("SCDynamicStoreSetValue(writer) failed");
		goto out;
	}
	printf("  writer set %s — running the run loop\n", SCR_KEY);

	/* run the run loop up to 5s; it returns once the source fires */
	(void) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 5.0, TRUE);

	if (!r.matched) {
		fail("no run-loop callback with the watched key within 5s");
		goto out;
	}
	printf("  run-loop callback fired with the watched key\n");

	/* tear the source back down */
	CFRunLoopRemoveSource(CFRunLoopGetCurrent(), rls, kCFRunLoopDefaultMode);
	CFRunLoopSourceInvalidate(rls);
	printf("  source removed + invalidated\n");

	printf("SC-RUNLOOP-OK: SCDynamicStore run-loop notifications work\n");
	rc = 0;

    out :
	fflush(stdout);
	if (rls != NULL)     CFRelease(rls);
	if (watcher != NULL) CFRelease(watcher);
	if (writer != NULL)  CFRelease(writer);
	CFRelease(watcherName);
	CFRelease(writerName);
	CFRelease(key);
	CFRelease(value);
	return rc;
}
