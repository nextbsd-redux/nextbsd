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
main(void)
{
	SCDynamicStoreRef store = NULL;
	CFStringRef session = NULL;
	char *cn = NULL, *hn = NULL, *lhn = NULL;
	char kn[KERN_LIMIT];
	int rc = 1;

	session = CFStringCreateWithCString(NULL, "hostnametest",
	    kCFStringEncodingUTF8);
	if (session == NULL) {
		(void)printf("HOSTNAMED-FAIL: CFStringCreate(session)\n");
		goto out;
	}
	store = SCDynamicStoreCreate(NULL, session, NULL, NULL);
	if (store == NULL) {
		(void)printf("HOSTNAMED-FAIL: SCDynamicStoreCreate "
		    "(configd unreachable?)\n");
		goto out;
	}

	cn = read_dict_string(store, "Setup:/System", "ComputerName");
	if (cn == NULL || cn[0] == '\0') {
		(void)printf("HOSTNAMED-FAIL: Setup:/System missing "
		    "ComputerName\n");
		goto out;
	}

	hn = read_dict_string(store, "Setup:/Network/HostNames", "HostName");
	if (hn == NULL || hn[0] == '\0') {
		(void)printf("HOSTNAMED-FAIL: Setup:/Network/HostNames "
		    "missing HostName\n");
		goto out;
	}

	lhn = read_dict_string(store, "Setup:/Network/HostNames",
	    "LocalHostName");
	if (lhn == NULL || lhn[0] == '\0') {
		(void)printf("HOSTNAMED-FAIL: Setup:/Network/HostNames "
		    "missing LocalHostName\n");
		goto out;
	}

	if (gethostname(kn, sizeof(kn)) != 0 || kn[0] == '\0') {
		(void)printf("HOSTNAMED-FAIL: gethostname(3) returned "
		    "nothing\n");
		goto out;
	}
	if (strcmp(kn, "Amnesiac") == 0) {
		(void)printf("HOSTNAMED-FAIL: gethostname(3) still "
		    "'Amnesiac' — hostnamed did not run or sethostname(2) "
		    "failed\n");
		goto out;
	}

	if (strcmp(cn, hn) != 0 || strcmp(cn, lhn) != 0 ||
	    strcmp(cn, kn) != 0) {
		(void)printf("HOSTNAMED-FAIL: sources disagree "
		    "(ComputerName='%s' HostName='%s' LocalHostName='%s' "
		    "kernel='%s')\n", cn, hn, lhn, kn);
		goto out;
	}

	(void)printf("HOSTNAMED-OK: hostname='%s' "
	    "(Setup:/System + Setup:/Network/HostNames + kernel all agree)\n",
	    cn);
	rc = 0;
out:
	free(cn);
	free(hn);
	free(lhn);
	if (store != NULL) CFRelease(store);
	if (session != NULL) CFRelease(session);
	return (rc);
}
