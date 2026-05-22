/*
 * scmultitest.c — libSystemConfiguration iter 4 batch get/set test
 * client.
 *
 * Exercises the batch store calls: SCDynamicStoreSetMultiple sets two
 * keys in one call; SCDynamicStoreCopyMultiple fetches them back by
 * key and by pattern; SCDynamicStoreSetMultiple then removes one. All
 * against the live configd. run.sh runs it and boot-test.sh gates on
 * the SC-MULTI-OK / SC-MULTI-FAIL marker.
 */

#include <SystemConfiguration/SCDynamicStore.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>

#define	SCM_KEY_A	"scmultitest:a"
#define	SCM_KEY_B	"scmultitest:b"
#define	SCM_PATTERN	"scmultitest:.*"

static void
fail(const char *msg)
{
	printf("SC-MULTI-FAIL: %s\n", msg);
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
	SCDynamicStoreRef	store	= NULL;
	CFStringRef		name;
	CFStringRef		keyA, keyB, valueA, valueB, pattern;
	CFMutableDictionaryRef	toSet	= NULL;
	CFMutableArrayRef	getKeys	= NULL;
	CFMutableArrayRef	patterns = NULL;
	CFMutableArrayRef	toRemove = NULL;
	CFDictionaryRef		got	= NULL;
	int			rc	= 1;

	printf("scmultitest: SCDynamicStore batch get/set\n");
	fflush(stdout);

	name	= mkstr("scmultitest");
	keyA	= mkstr(SCM_KEY_A);
	keyB	= mkstr(SCM_KEY_B);
	valueA	= mkstr("value-a");
	valueB	= mkstr("value-b");
	pattern	= mkstr(SCM_PATTERN);

	store = SCDynamicStoreCreate(NULL, name, NULL, NULL);
	if (store == NULL) {
		fail("SCDynamicStoreCreate failed");
		goto out;
	}

	/* SetMultiple — set two keys in one call */
	toSet = CFDictionaryCreateMutable(NULL, 0,
					  &kCFTypeDictionaryKeyCallBacks,
					  &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(toSet, keyA, valueA);
	CFDictionarySetValue(toSet, keyB, valueB);
	if (!SCDynamicStoreSetMultiple(store, toSet, NULL, NULL)) {
		fail("SCDynamicStoreSetMultiple(set) failed");
		goto out;
	}
	printf("  SetMultiple: set 2 keys\n");

	/* CopyMultiple by key */
	getKeys = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	CFArrayAppendValue(getKeys, keyA);
	CFArrayAppendValue(getKeys, keyB);
	got = SCDynamicStoreCopyMultiple(store, getKeys, NULL);
	if (got == NULL) {
		fail("SCDynamicStoreCopyMultiple(keys) returned NULL");
		goto out;
	}
	{
		CFTypeRef	gotA	= CFDictionaryGetValue(got, keyA);
		CFTypeRef	gotB	= CFDictionaryGetValue(got, keyB);

		if ((gotA == NULL) || (gotB == NULL) ||
		    !CFEqual(gotA, valueA) || !CFEqual(gotB, valueB)) {
			fail("CopyMultiple(keys) returned wrong values");
			goto out;
		}
	}
	CFRelease(got);
	got = NULL;
	printf("  CopyMultiple: fetched 2 keys by key\n");

	/* CopyMultiple by pattern */
	patterns = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	CFArrayAppendValue(patterns, pattern);
	got = SCDynamicStoreCopyMultiple(store, NULL, patterns);
	if ((got == NULL) ||
	    (CFDictionaryGetValue(got, keyA) == NULL) ||
	    (CFDictionaryGetValue(got, keyB) == NULL)) {
		fail("CopyMultiple(pattern) missing a matched key");
		goto out;
	}
	CFRelease(got);
	got = NULL;
	printf("  CopyMultiple: fetched keys by pattern\n");

	/* SetMultiple — remove one key */
	toRemove = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	CFArrayAppendValue(toRemove, keyA);
	if (!SCDynamicStoreSetMultiple(store, NULL, toRemove, NULL)) {
		fail("SCDynamicStoreSetMultiple(remove) failed");
		goto out;
	}
	got = SCDynamicStoreCopyMultiple(store, getKeys, NULL);
	if (got == NULL) {
		fail("CopyMultiple after remove returned NULL");
		goto out;
	}
	if (CFDictionaryGetValue(got, keyA) != NULL) {
		fail("removed key still present");
		goto out;
	}
	if (CFDictionaryGetValue(got, keyB) == NULL) {
		fail("CopyMultiple dropped the key that was kept");
		goto out;
	}
	CFRelease(got);
	got = NULL;
	printf("  SetMultiple: removed a key\n");

	printf("SC-MULTI-OK: SCDynamicStore batch get/set works\n");
	rc = 0;

    out :
	fflush(stdout);
	if (got != NULL)      CFRelease(got);
	if (store != NULL)    CFRelease(store);
	if (toSet != NULL)    CFRelease(toSet);
	if (getKeys != NULL)  CFRelease(getKeys);
	if (patterns != NULL) CFRelease(patterns);
	if (toRemove != NULL) CFRelease(toRemove);
	CFRelease(name);
	CFRelease(keyA);
	CFRelease(keyB);
	CFRelease(valueA);
	CFRelease(valueB);
	CFRelease(pattern);
	return rc;
}
