/*
 * hostnamedhcpset — CI fixture helper for hostnamed iter 3a / 3b.
 *
 * Usage:  hostnamedhcpset <option-12-value>
 *         hostnamedhcpset --clear
 *
 * With a value: finds the State:/Network/Service/<UUID>/DHCP dict that
 * ipconfigd already published (issue #88), adds Option_12=<argv[1]> to
 * it, and writes the augmented dict back to SCDynamicStore. The next
 * hostnamed run will read that value via its DHCP tier (try_dhcp) and
 * use it, proving Tier-3a fires.
 *
 * With --clear: enumerates all matching /DHCP dicts and removes
 * Option_12 from each, so a previously-injected fixture does not
 * short-circuit later tiers (iter 3b's ROUND 4 needs this to exercise
 * the mDNS path with the DHCP tier guaranteed empty).
 *
 * Why we modify ipconfigd's existing /DHCP key rather than minting our
 * own: the live key is already shaped correctly (InterfaceName +
 * LeaseStartTime) and has a real UUID derived from the active NIC MAC.
 * Synthesizing one would duplicate sc_publish.c's UUID logic; piggy-
 * backing keeps the fixture small and the test against a real surface.
 *
 * Not shipped to real images (CI-only tool); lives next to
 * hostnametest + hostnameprefset under /usr/tests/freebsd-launchd-mach/.
 *
 * Issue: #90
 */

#include <SystemConfiguration/SCDynamicStore.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Clear Option_12 from every matching /DHCP dict. Returns 0 on success
 * (even when no dicts contained Option_12 — the empty-key case is fine
 * for run.sh which calls --clear unconditionally). Returns 1 on SCDS
 * error. */
static int
clear_all_option12(SCDynamicStoreRef store, CFStringRef pattern,
    CFStringRef k_opt12)
{
	CFArrayRef keys;
	CFIndex i, n;
	int cleared = 0;

	keys = SCDynamicStoreCopyKeyList(store, pattern);
	if (keys == NULL)
		return (0);	/* no /DHCP keys at all — nothing to clear */
	n = CFArrayGetCount(keys);
	for (i = 0; i < n; i++) {
		CFStringRef key;
		CFPropertyListRef plist;
		CFMutableDictionaryRef dict;

		key = (CFStringRef)CFArrayGetValueAtIndex(keys, i);
		if (key == NULL)
			continue;
		plist = SCDynamicStoreCopyValue(store, key);
		if (plist == NULL)
			continue;
		if (CFGetTypeID(plist) != CFDictionaryGetTypeID() ||
		    !CFDictionaryContainsKey((CFDictionaryRef)plist, k_opt12)) {
			CFRelease(plist);
			continue;
		}
		dict = CFDictionaryCreateMutableCopy(NULL, 0,
		    (CFDictionaryRef)plist);
		CFRelease(plist);
		if (dict == NULL)
			continue;
		CFDictionaryRemoveValue(dict, k_opt12);
		if (SCDynamicStoreSetValue(store, key, dict)) {
			char keybuf[256];
			if (CFStringGetCString(key, keybuf, sizeof(keybuf),
			    kCFStringEncodingUTF8))
				(void)printf("hostnamedhcpset: cleared "
				    "Option_12 from %s\n", keybuf);
			cleared++;
		}
		CFRelease(dict);
	}
	CFRelease(keys);
	if (cleared == 0)
		(void)printf("hostnamedhcpset: no /DHCP dict carried "
		    "Option_12 (already clear)\n");
	return (0);
}

int
main(int argc, char **argv)
{
	SCDynamicStoreRef store = NULL;
	CFStringRef session = NULL, pattern = NULL, k_opt12 = NULL;
	CFStringRef value = NULL, key = NULL;
	CFArrayRef keys = NULL;
	CFPropertyListRef plist = NULL;
	CFMutableDictionaryRef dict = NULL;
	CFIndex n;
	int clear_mode = 0;
	int rc = 1;

	if (argc != 2 || argv[1][0] == '\0') {
		(void)fprintf(stderr,
		    "usage: %s <option-12-value>\n"
		    "       %s --clear\n", argv[0], argv[0]);
		return (2);
	}
	if (strcmp(argv[1], "--clear") == 0)
		clear_mode = 1;

	session = CFStringCreateWithCString(NULL, "hostnamedhcpset",
	    kCFStringEncodingUTF8);
	pattern = CFStringCreateWithCString(NULL,
	    "State:/Network/Service/[^/]+/DHCP", kCFStringEncodingUTF8);
	k_opt12 = CFStringCreateWithCString(NULL, "Option_12",
	    kCFStringEncodingUTF8);
	if (!clear_mode)
		value = CFStringCreateWithCString(NULL, argv[1],
		    kCFStringEncodingUTF8);
	if (session == NULL || pattern == NULL || k_opt12 == NULL ||
	    (!clear_mode && value == NULL)) {
		(void)fprintf(stderr, "CFString allocation failed\n");
		goto out;
	}

	store = SCDynamicStoreCreate(NULL, session, NULL, NULL);
	if (store == NULL) {
		(void)fprintf(stderr, "SCDynamicStoreCreate failed: %s\n",
		    SCErrorString(SCError()));
		goto out;
	}

	if (clear_mode) {
		rc = clear_all_option12(store, pattern, k_opt12);
		goto out;
	}

	keys = SCDynamicStoreCopyKeyList(store, pattern);
	if (keys == NULL || (n = CFArrayGetCount(keys)) == 0) {
		(void)fprintf(stderr,
		    "no State:/Network/Service/<UUID>/DHCP key found "
		    "(is ipconfigd running and BOUND?)\n");
		goto out;
	}

	/* Use the first matching key. iter 3a doesn't yet pick the
	 * primary service — fine for a CI environment with a single
	 * em0 / virtio NIC; iter 3b will do State:/Network/Global/IPv4
	 * lookup to pick the active service. */
	key = (CFStringRef)CFArrayGetValueAtIndex(keys, 0);
	if (key == NULL) {
		(void)fprintf(stderr, "key list returned NULL entry\n");
		goto out;
	}
	CFRetain(key);

	plist = SCDynamicStoreCopyValue(store, key);
	if (plist == NULL ||
	    CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
		(void)fprintf(stderr, "existing /DHCP key has no dict value\n");
		goto out;
	}

	dict = CFDictionaryCreateMutableCopy(NULL, 0,
	    (CFDictionaryRef)plist);
	if (dict == NULL) {
		(void)fprintf(stderr,
		    "CFDictionaryCreateMutableCopy failed\n");
		goto out;
	}
	CFDictionarySetValue(dict, k_opt12, value);

	if (!SCDynamicStoreSetValue(store, key, dict)) {
		(void)fprintf(stderr,
		    "SCDynamicStoreSetValue failed: %s\n",
		    SCErrorString(SCError()));
		goto out;
	}

	{
		char keybuf[256];
		if (CFStringGetCString(key, keybuf, sizeof(keybuf),
		    kCFStringEncodingUTF8)) {
			(void)printf("hostnamedhcpset: wrote Option_12='%s' "
			    "into %s\n", argv[1], keybuf);
		} else {
			(void)printf("hostnamedhcpset: wrote Option_12='%s'\n",
			    argv[1]);
		}
	}
	rc = 0;
out:
	if (dict != NULL) CFRelease(dict);
	if (plist != NULL) CFRelease(plist);
	if (key != NULL) CFRelease(key);
	if (keys != NULL) CFRelease(keys);
	if (store != NULL) CFRelease(store);
	if (value != NULL) CFRelease(value);
	if (k_opt12 != NULL) CFRelease(k_opt12);
	if (pattern != NULL) CFRelease(pattern);
	if (session != NULL) CFRelease(session);
	return (rc);
}
