/*
 * hostnametest — iter 1 readback smoke test for hostnamed.
 *
 * Verifies the three publish surfaces hostnamed writes at boot:
 *   1. SCDynamicStore "Setup:/System"             (ComputerName)
 *   2. SCDynamicStore "Setup:/Network/HostNames"  (HostName + LocalHostName)
 *   3. gethostname(3)                             (kernel-side via sethostname)
 *
 * Prints HOSTNAMED-OK / HOSTNAMED-FAIL on a single line — same shape
 * as the other iter-1 markers (DA-BOOT, IPCFG-BOOT, MDNS-BOOT). The
 * boot-test.sh expect block in tests/ gates on this marker.
 *
 * Pass criteria:
 *   - Setup:/System dict exists and has a non-empty ComputerName
 *   - Setup:/Network/HostNames dict exists and has non-empty
 *     HostName + LocalHostName
 *   - gethostname(3) returns a non-empty string that is NOT "Amnesiac"
 *     (FreeBSD's default-unset placeholder — proves hostnamed did
 *     actually replace it)
 *   - All three sources report the SAME value (sanity)
 *
 * Issue: #63
 */

#include <SystemConfiguration/SCDynamicStore.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define KERN_LIMIT	256

static char *
cf_to_cstr(CFStringRef s)
{
	CFIndex len, max;
	char *buf;

	if (s == NULL || CFGetTypeID(s) != CFStringGetTypeID())
		return (NULL);
	len = CFStringGetLength(s);
	max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
	buf = malloc((size_t)max);
	if (buf == NULL)
		return (NULL);
	if (!CFStringGetCString(s, buf, max, kCFStringEncodingUTF8)) {
		free(buf);
		return (NULL);
	}
	return (buf);
}

static char *
read_dict_string(SCDynamicStoreRef store, const char *key_str,
    const char *member)
{
	CFStringRef key = NULL, k_member = NULL;
	CFPropertyListRef plist = NULL;
	CFStringRef cf_val;
	char *out = NULL;

	key = CFStringCreateWithCString(NULL, key_str, kCFStringEncodingUTF8);
	k_member = CFStringCreateWithCString(NULL, member,
	    kCFStringEncodingUTF8);
	if (key == NULL || k_member == NULL)
		goto out;

	plist = SCDynamicStoreCopyValue(store, key);
	if (plist == NULL)
		goto out;
	if (CFGetTypeID(plist) != CFDictionaryGetTypeID())
		goto out;
	cf_val = CFDictionaryGetValue((CFDictionaryRef)plist, k_member);
	out = cf_to_cstr(cf_val);
out:
	if (plist != NULL) CFRelease(plist);
	if (k_member != NULL) CFRelease(k_member);
	if (key != NULL) CFRelease(key);
	return (out);
}

int
main(int argc, char **argv)
{
	SCDynamicStoreRef store = NULL;
	CFStringRef session = NULL;
	char *cn = NULL, *hn = NULL, *lhn = NULL;
	char kn[KERN_LIMIT];
	const char *expected;
	const char *ok_marker, *fail_marker, *mode_label;
	int rc = 1;

	/*
	 * Two modes:
	 *   hostnametest                — iter 1 regression check: verify
	 *                                 sources agree AND value isn't
	 *                                 "Amnesiac". Emits HOSTNAMED-OK /
	 *                                 HOSTNAMED-FAIL.
	 *   hostnametest <expected>     — iter 2 (issue #86) Tier-2 read
	 *                                 check: verify sources agree AND
	 *                                 the published value equals
	 *                                 <expected> (the fixture that
	 *                                 hostnameprefset wrote to SCPrefs
	 *                                 before hostnamed ran). Emits
	 *                                 HOSTNAMED-PREFS-OK /
	 *                                 HOSTNAMED-PREFS-FAIL.
	 */
	expected = (argc == 2) ? argv[1] : NULL;
	if (expected != NULL) {
		ok_marker   = "HOSTNAMED-PREFS-OK";
		fail_marker = "HOSTNAMED-PREFS-FAIL";
		mode_label  = "iter 2 SCPrefs read";
	} else {
		ok_marker   = "HOSTNAMED-OK";
		fail_marker = "HOSTNAMED-FAIL";
		mode_label  = "iter 1 synthesis";
	}

	session = CFStringCreateWithCString(NULL, "hostnametest",
	    kCFStringEncodingUTF8);
	if (session == NULL) {
		(void)printf("%s: CFStringCreate(session)\n", fail_marker);
		goto out;
	}
	store = SCDynamicStoreCreate(NULL, session, NULL, NULL);
	if (store == NULL) {
		(void)printf("%s: SCDynamicStoreCreate "
		    "(configd unreachable?)\n", fail_marker);
		goto out;
	}

	cn = read_dict_string(store, "Setup:/System", "ComputerName");
	if (cn == NULL || cn[0] == '\0') {
		(void)printf("%s: Setup:/System missing ComputerName\n",
		    fail_marker);
		goto out;
	}

	hn = read_dict_string(store, "Setup:/Network/HostNames", "HostName");
	if (hn == NULL || hn[0] == '\0') {
		(void)printf("%s: Setup:/Network/HostNames missing "
		    "HostName\n", fail_marker);
		goto out;
	}

	lhn = read_dict_string(store, "Setup:/Network/HostNames",
	    "LocalHostName");
	if (lhn == NULL || lhn[0] == '\0') {
		(void)printf("%s: Setup:/Network/HostNames missing "
		    "LocalHostName\n", fail_marker);
		goto out;
	}

	if (gethostname(kn, sizeof(kn)) != 0 || kn[0] == '\0') {
		(void)printf("%s: gethostname(3) returned nothing\n",
		    fail_marker);
		goto out;
	}
	if (expected == NULL && strcmp(kn, "Amnesiac") == 0) {
		(void)printf("%s: gethostname(3) still 'Amnesiac' — "
		    "hostnamed did not run or sethostname(2) failed\n",
		    fail_marker);
		goto out;
	}

	if (strcmp(cn, hn) != 0 || strcmp(cn, lhn) != 0 ||
	    strcmp(cn, kn) != 0) {
		(void)printf("%s: sources disagree (ComputerName='%s' "
		    "HostName='%s' LocalHostName='%s' kernel='%s')\n",
		    fail_marker, cn, hn, lhn, kn);
		goto out;
	}

	if (expected != NULL && strcmp(cn, expected) != 0) {
		(void)printf("%s: expected='%s' but published='%s' — "
		    "Tier-2 SCPrefs read did not fire (synthesis still ran?)\n",
		    fail_marker, expected, cn);
		goto out;
	}

	(void)printf("%s: hostname='%s' (%s; "
	    "Setup:/System + Setup:/Network/HostNames + kernel all agree)\n",
	    ok_marker, cn, mode_label);
	rc = 0;
out:
	free(cn);
	free(hn);
	free(lhn);
	if (store != NULL) CFRelease(store);
	if (session != NULL) CFRelease(session);
	return (rc);
}
