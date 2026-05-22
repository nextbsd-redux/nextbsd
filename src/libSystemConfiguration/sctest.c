/*
 * sctest.c — libSystemConfiguration iter 1 round-trip test client.
 *
 * Exercises the CoreFoundation-typed SCDynamicStore* client API
 * against the live configd daemon: open a session, set / get / add /
 * remove property-list values and list keys — the whole synchronous
 * store surface, going through libSystemConfiguration's MIG client
 * stubs rather than speaking config.defs by hand (the way configtest
 * does). run.sh runs it and boot-test.sh gates on the SC-STORE-OK /
 * SC-STORE-FAIL marker it prints.
 */

#include <SystemConfiguration/SCDynamicStore.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>

#define	SCT_KEY1	"sctest:key1"
#define	SCT_KEY2	"sctest:key2"
#define	SCT_KEY3	"sctest:key3"
#define	SCT_PATTERN	"sctest:.*"

static void
fail(const char *msg)
{
	printf("SC-STORE-FAIL: %s\n", msg);
	fflush(stdout);
}

/* CFStringCreateWithCString wrapper — avoids CFSTR()/-fconstant-cfstrings */
static CFStringRef
mkstr(const char *s)
{
	return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
}

int
main(void)
{
	SCDynamicStoreRef	store;
	CFStringRef		name;
	CFStringRef		k1, k2, k3, pattern;
	CFStringRef		v1;
	CFMutableDictionaryRef	v2;
	CFPropertyListRef	got;
	CFArrayRef		keys;
	CFRange			range;
	int			rc	= 1;

	printf("sctest: SCDynamicStore client round-trip\n");
	fflush(stdout);

	name	= mkstr("sctest");
	k1	= mkstr(SCT_KEY1);
	k2	= mkstr(SCT_KEY2);
	k3	= mkstr(SCT_KEY3);
	pattern	= mkstr(SCT_PATTERN);
	v1	= mkstr("value-one");

	v2 = CFDictionaryCreateMutable(NULL, 0,
				       &kCFTypeDictionaryKeyCallBacks,
				       &kCFTypeDictionaryValueCallBacks);
	{
		CFStringRef	dk = mkstr("field");
		CFStringRef	dv = mkstr("dict-value");
		CFDictionarySetValue(v2, dk, dv);
		CFRelease(dk);
		CFRelease(dv);
	}

	/* open a session with configd */
	store = SCDynamicStoreCreate(NULL, name, NULL, NULL);
	if (store == NULL) {
		fail("SCDynamicStoreCreate returned NULL");
		printf("  SCError: %s\n", SCErrorString(SCError()));
		goto out;
	}
	printf("  SCDynamicStoreCreate: session opened\n");

	/* CFGetTypeID round-trip — SCDynamicStoreRef is a real CF type */
	if (CFGetTypeID(store) != SCDynamicStoreGetTypeID()) {
		fail("store is not a SCDynamicStore CFType");
		goto out;
	}

	/* SetValue / CopyValue — CFString value */
	if (!SCDynamicStoreSetValue(store, k1, v1)) {
		fail("SCDynamicStoreSetValue(key1) failed");
		printf("  SCError: %s\n", SCErrorString(SCError()));
		goto out;
	}
	got = SCDynamicStoreCopyValue(store, k1);
	if (got == NULL) {
		fail("SCDynamicStoreCopyValue(key1) returned NULL");
		goto out;
	}
	if (!CFEqual(got, v1)) {
		fail("key1 value did not round-trip");
		CFRelease(got);
		goto out;
	}
	CFRelease(got);
	printf("  SetValue/CopyValue: CFString round-trip OK\n");

	/* SetValue / CopyValue — CFDictionary value */
	if (!SCDynamicStoreSetValue(store, k2, v2)) {
		fail("SCDynamicStoreSetValue(key2) failed");
		goto out;
	}
	got = SCDynamicStoreCopyValue(store, k2);
	if (got == NULL || !CFEqual(got, v2)) {
		fail("key2 dictionary value did not round-trip");
		if (got != NULL) CFRelease(got);
		goto out;
	}
	CFRelease(got);
	printf("  SetValue/CopyValue: CFDictionary round-trip OK\n");

	/* AddValue — succeeds once, then fails with kSCStatusKeyExists */
	if (!SCDynamicStoreAddValue(store, k3, v1)) {
		fail("SCDynamicStoreAddValue(key3) failed");
		goto out;
	}
	if (SCDynamicStoreAddValue(store, k3, v1)) {
		fail("SCDynamicStoreAddValue(key3) should have failed (exists)");
		goto out;
	}
	if (SCError() != kSCStatusKeyExists) {
		fail("re-add did not report kSCStatusKeyExists");
		goto out;
	}
	printf("  AddValue: add + duplicate-rejected OK\n");

	/* CopyKeyList — all three sctest keys must be present */
	keys = SCDynamicStoreCopyKeyList(store, pattern);
	if (keys == NULL) {
		fail("SCDynamicStoreCopyKeyList returned NULL");
		goto out;
	}
	range = CFRangeMake(0, CFArrayGetCount(keys));
	if (!CFArrayContainsValue(keys, range, k1) ||
	    !CFArrayContainsValue(keys, range, k2) ||
	    !CFArrayContainsValue(keys, range, k3)) {
		fail("CopyKeyList missing one of the sctest keys");
		CFRelease(keys);
		goto out;
	}
	CFRelease(keys);
	printf("  CopyKeyList: all sctest keys listed OK\n");

	/* RemoveValue, then CopyValue must miss with kSCStatusNoKey */
	if (!SCDynamicStoreRemoveValue(store, k1)) {
		fail("SCDynamicStoreRemoveValue(key1) failed");
		goto out;
	}
	got = SCDynamicStoreCopyValue(store, k1);
	if (got != NULL) {
		fail("CopyValue(key1) succeeded after remove");
		CFRelease(got);
		goto out;
	}
	if (SCError() != kSCStatusNoKey) {
		fail("CopyValue after remove did not report kSCStatusNoKey");
		goto out;
	}
	printf("  RemoveValue: remove + miss-after-remove OK\n");

	printf("SC-STORE-OK: SCDynamicStore client round-trip works\n");
	rc = 0;

    out :
	fflush(stdout);
	if (store != NULL) CFRelease(store);
	CFRelease(name);
	CFRelease(k1);
	CFRelease(k2);
	CFRelease(k3);
	CFRelease(pattern);
	CFRelease(v1);
	CFRelease(v2);
	return rc;
}
