/*
 * hostnamedhcpset — CI fixture helper for hostnamed iter 3a.
 *
 * Usage:  hostnamedhcpset <option-12-value>
 *
 * Finds the State:/Network/Service/<UUID>/DHCP dict that ipconfigd
 * already published (issue #88), adds Option_12=<argv[1]> to it, and
 * writes the augmented dict back to SCDynamicStore. The next hostnamed
 * run will read that value via its DHCP tier (try_dhcp) and use it,
 * proving Tier-3a fires.
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
	int rc = 1;

	if (argc != 2 || argv[1][0] == '\0') {
		(void)fprintf(stderr,
		    "usage: %s <option-12-value>\n", argv[0]);
		return (2);
	}

	session = CFStringCreateWithCString(NULL, "hostnamedhcpset",
	    kCFStringEncodingUTF8);
	pattern = CFStringCreateWithCString(NULL,
	    "State:/Network/Service/[^/]+/DHCP", kCFStringEncodingUTF8);
	k_opt12 = CFStringCreateWithCString(NULL, "Option_12",
	    kCFStringEncodingUTF8);
	value = CFStringCreateWithCString(NULL, argv[1],
	    kCFStringEncodingUTF8);
	if (session == NULL || pattern == NULL || k_opt12 == NULL ||
	    value == NULL) {
		(void)fprintf(stderr, "CFString allocation failed\n");
		goto out;
	}

	store = SCDynamicStoreCreate(NULL, session, NULL, NULL);
	if (store == NULL) {
		(void)fprintf(stderr, "SCDynamicStoreCreate failed: %s\n",
		    SCErrorString(SCError()));
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
