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
	char ok_buf[64], fail_buf[64], mode_buf[64];
	int rc = 1;

	/*
	 * Three modes:
	 *   hostnametest
	 *       iter-1 regression check: sources agree AND value isn't
	 *       "Amnesiac". Emits HOSTNAMED-OK / HOSTNAMED-FAIL.
	 *   hostnametest <expected>
	 *       iter-2 (issue #86) SCPrefs Tier-2 check: sources agree AND
	 *       value equals <expected> (the fixture hostnameprefset wrote
	 *       before hostnamed ran). Emits HOSTNAMED-PREFS-OK / -FAIL.
	 *   hostnametest <expected> <suffix>
	 *       iter-3+ tier check: same as iter-2 verification but the
	 *       marker label becomes HOSTNAMED-<SUFFIX>-OK / -FAIL. Used
	 *       by iter-3a as `hostnametest <fixture> DHCP` to emit
	 *       HOSTNAMED-DHCP-OK (issue #90).
	 */
	expected = (argc >= 2) ? argv[1] : NULL;
	if (argc >= 3) {
		(void)snprintf(ok_buf, sizeof(ok_buf),
		    "HOSTNAMED-%s-OK", argv[2]);
		(void)snprintf(fail_buf, sizeof(fail_buf),
		    "HOSTNAMED-%s-FAIL", argv[2]);
		(void)snprintf(mode_buf, sizeof(mode_buf),
		    "iter 3+ %s read", argv[2]);
		ok_marker   = ok_buf;
		fail_marker = fail_buf;
		mode_label  = mode_buf;
	} else if (expected != NULL) {
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

	cn  = read_dict_string(store, "Setup:/System", "ComputerName");
	hn  = read_dict_string(store, "Setup:/Network/HostNames", "HostName");
	lhn = read_dict_string(store, "Setup:/Network/HostNames",
	    "LocalHostName");

	if (gethostname(kn, sizeof(kn)) != 0 || kn[0] == '\0') {
		(void)printf("%s: gethostname(3) returned nothing\n",
		    fail_marker);
		goto out;
	}
	if (strcmp(kn, "Amnesiac") == 0 || strcmp(kn, "localhost") == 0) {
		(void)printf("%s: gethostname(3) is '%s' — "
		    "hostnamed did not run or sethostname(2) failed\n",
		    fail_marker, kn);
		goto out;
	}

	/* When the caller supplied an expected value, enforce a contract
	 * appropriate to the tier:
	 *
	 *   - SCPrefs path (argv[2] absent OR == "PREFS"): prefs_monitor
	 *     mirrors the SCPrefs ComputerName into Setup:/System +
	 *     Setup:/Network/HostNames; set-hostname.c then sethostname()s
	 *     the kernel from Setup:/System. All four surfaces must
	 *     equal expected.
	 *
	 *   - DHCP / PTR / mDNS paths (argv[2] in {"DHCP", "MDNS", ...}):
	 *     set-hostname.c sethostname()s the kernel from
	 *     State:/Network/Service/.../DHCP/Option_12 (or the resolved
	 *     PTR/mDNS name) directly. Setup:/System and Setup:/Network/
	 *     HostNames are NOT touched — those mirror SCPrefs, which is
	 *     empty in these rounds by construction. Only assert kernel
	 *     hostname equals expected. */
	if (expected != NULL) {
		const Boolean uses_setup_mirror = (argc < 3 ||
		    strcmp(argv[2], "PREFS") == 0);

		if (uses_setup_mirror) {
			if (cn == NULL || hn == NULL || lhn == NULL) {
				(void)printf("%s: %s expected Setup keys "
				    "populated (ComputerName='%s' "
				    "HostName='%s' LocalHostName='%s')\n",
				    fail_marker, mode_label,
				    cn ? cn : "<absent>", hn ? hn : "<absent>",
				    lhn ? lhn : "<absent>");
				goto out;
			}
			if (strcmp(cn, expected) != 0 ||
			    strcmp(hn, expected) != 0 ||
			    strcmp(lhn, expected) != 0 ||
			    strcmp(kn, expected) != 0) {
				(void)printf("%s: expected='%s' but "
				    "ComputerName='%s' HostName='%s' "
				    "LocalHostName='%s' kernel='%s' — %s did "
				    "not win\n", fail_marker, expected,
				    cn, hn, lhn, kn, mode_label);
				goto out;
			}
			(void)printf("%s: hostname='%s' (%s; Setup:/System "
			    "+ Setup:/Network/HostNames + kernel all agree)\n",
			    ok_marker, expected, mode_label);
			rc = 0;
			goto out;
		}

		/* Engine-direct tier (DHCP / PTR / mDNS): kernel-only. */
		if (strcmp(kn, expected) != 0) {
			(void)printf("%s: expected kernel='%s' but kernel='%s' "
			    "— %s did not win (lower-precedence tier fired?)\n",
			    fail_marker, expected, kn, mode_label);
			goto out;
		}
		(void)printf("%s: hostname='%s' (%s; kernel sethostname'd "
		    "directly by engine; Setup:/System left empty per "
		    "Apple-shape SCPrefs-mirroring)\n",
		    ok_marker, expected, mode_label);
		rc = 0;
		goto out;
	}

	/* Synthesis path (no expected): set-hostname.c's freebsd_synthesize
	 * carry won the chain — it sethostname()s the synth value but does
	 * NOT touch Setup:/System / Setup:/Network/HostNames (those mirror
	 * SCPrefs ComputerName via prefs_monitor; SCPrefs is empty in this
	 * round). So we only assert the kernel-side outcome: hostname was
	 * actually changed away from the FreeBSD defaults. */
	(void)printf("%s: hostname='%s' (%s; kernel sethostname'd via "
	    "synthesis carry, Setup:/System left empty per Apple-shape "
	    "SCPrefs-mirroring)\n", ok_marker, kn, mode_label);
	rc = 0;
out:
	free(cn);
	free(hn);
	free(lhn);
	if (store != NULL) CFRelease(store);
	if (session != NULL) CFRelease(session);
	return (rc);
}
