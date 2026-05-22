/*
 * scplinktest.c — libSystemConfiguration SCNetworkConfiguration iter 1
 * test client: the SCPreferences path link / unique-child accessors.
 *
 * These are the SCPreferences pieces SCNetworkConfiguration is built on
 * — SCNetworkServiceCreate mints entries with SCPreferencesPathCreate
 * UniqueChild, and SCNetworkSet references its services through
 * __LINK__ dictionaries. This client exercises:
 *
 *   - SCPreferencesPathCreateUniqueChild: a fresh empty child entry;
 *   - SCPreferencesPathSetLink / GetLink: store + read a raw link;
 *   - SCPreferencesPathGetValue through a link, both at the leaf and
 *     mid-path (a component walked *through*);
 *   - commit / reopen: the link survives a round-trip to the file;
 *   - SetLink rejects a dangling target;
 *   - a __LINK__ cycle is caught with kSCStatusMaxLink.
 *
 * run.sh runs it and boot-test.sh gates on the SC-PLINK-OK / -FAIL
 * marker.
 */

#include <SystemConfiguration/SystemConfiguration.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>

#define	SCT_ID		"scplinktest.plist"
#define	SCT_PREFIX	"/NetworkServices"
#define	SCT_LINK	"/Sets/set1/Network/Service/link0"

static void
fail(const char *msg)
{
	printf("SC-PLINK-FAIL: %s\n", msg);
	fflush(stdout);
}

static CFStringRef
mkstr(const char *s)
{
	return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
}

/* a single-entry dictionary { key: value } */
static CFDictionaryRef
mkdict1(const char *key, const char *value)
{
	CFStringRef		k	= mkstr(key);
	CFStringRef		v	= mkstr(value);
	CFDictionaryRef		d;
	const void		*kk[1]	= { k };
	const void		*vv[1]	= { v };

	d = CFDictionaryCreate(NULL, kk, vv, 1,
			       &kCFTypeDictionaryKeyCallBacks,
			       &kCFTypeDictionaryValueCallBacks);
	CFRelease(k);
	CFRelease(v);
	return d;
}

/* TRUE iff dict[key] is the string `want` */
static Boolean
dict_has(CFDictionaryRef dict, const char *key, const char *want)
{
	CFStringRef	k	= mkstr(key);
	CFStringRef	w	= mkstr(want);
	CFTypeRef	v;
	Boolean		ok;

	v  = (dict != NULL) ? CFDictionaryGetValue(dict, k) : NULL;
	ok = (v != NULL) && CFEqual(v, w);
	CFRelease(k);
	CFRelease(w);
	return ok;
}

int
main(void)
{
	SCPreferencesRef	prefs	= NULL;
	CFStringRef		name, prefsID;
	CFStringRef		prefix, linkPath, ipv4Path = NULL;
	CFStringRef		child	= NULL;
	CFStringRef		gotLink;
	CFDictionaryRef		svc	= NULL, ipv4 = NULL, got;
	int			rc	= 1;

	printf("scplinktest: SCPreferences path links + unique child\n");
	fflush(stdout);

	name	 = mkstr("scplinktest");
	prefsID	 = mkstr(SCT_ID);
	prefix	 = mkstr(SCT_PREFIX);
	linkPath = mkstr(SCT_LINK);
	svc	 = mkdict1("UserDefinedName", "TestService");
	ipv4	 = mkdict1("Method", "DHCP");

	prefs = SCPreferencesCreate(NULL, name, prefsID);
	if (prefs == NULL) {
		fail("SCPreferencesCreate failed");
		goto out;
	}

	/* a fresh, uniquely-named child entry under /NetworkServices */
	child = SCPreferencesPathCreateUniqueChild(prefs, prefix);
	if (child == NULL) {
		fail("SCPreferencesPathCreateUniqueChild returned NULL");
		goto out;
	}
	if (!CFStringHasPrefix(child, prefix)) {
		fail("unique child path not rooted at the prefix");
		goto out;
	}
	got = SCPreferencesPathGetValue(prefs, child);
	if ((got == NULL) || (CFDictionaryGetCount(got) != 0)) {
		fail("unique child is not an empty dictionary");
		goto out;
	}
	printf("  PathCreateUniqueChild: created an entry\n");

	/* give the entry some content + a nested IPv4 sub-entity */
	if (!SCPreferencesPathSetValue(prefs, child, svc)) {
		fail("SCPreferencesPathSetValue (child) failed");
		goto out;
	}
	ipv4Path = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@/IPv4"),
					    child);
	if (!SCPreferencesPathSetValue(prefs, ipv4Path, ipv4)) {
		fail("SCPreferencesPathSetValue (child/IPv4) failed");
		goto out;
	}

	/* link a Sets path at the service entry */
	if (!SCPreferencesPathSetLink(prefs, linkPath, child)) {
		fail("SCPreferencesPathSetLink failed");
		goto out;
	}
	gotLink = SCPreferencesPathGetLink(prefs, linkPath);
	if ((gotLink == NULL) || !CFEqual(gotLink, child)) {
		fail("SCPreferencesPathGetLink did not round-trip");
		goto out;
	}
	printf("  PathSetLink/GetLink: link round-trips\n");

	/* GetValue at the link path follows the link to the target */
	got = SCPreferencesPathGetValue(prefs, linkPath);
	if (!dict_has(got, "UserDefinedName", "TestService")) {
		fail("GetValue did not follow the leaf link");
		goto out;
	}

	/* GetValue *through* the link resolves a mid-path component */
	{
		CFStringRef	deep;

		deep = CFStringCreateWithFormat(NULL, NULL,
						CFSTR("%@/IPv4"), linkPath);
		got  = SCPreferencesPathGetValue(prefs, deep);
		CFRelease(deep);
		if (!dict_has(got, "Method", "DHCP")) {
			fail("GetValue did not follow a mid-path link");
			goto out;
		}
	}
	printf("  PathGetValue: leaf + mid-path link resolution OK\n");

	/* commit, re-open, confirm the link persisted */
	if (!SCPreferencesCommitChanges(prefs)) {
		fail("SCPreferencesCommitChanges failed");
		goto out;
	}
	CFRelease(prefs);
	prefs = SCPreferencesCreate(NULL, name, prefsID);
	if (prefs == NULL) {
		fail("SCPreferencesCreate (reopen) failed");
		goto out;
	}
	gotLink = SCPreferencesPathGetLink(prefs, linkPath);
	if ((gotLink == NULL) || !CFEqual(gotLink, child)) {
		fail("link did not persist across commit/reopen");
		goto out;
	}
	{
		CFStringRef	deep;

		deep = CFStringCreateWithFormat(NULL, NULL,
						CFSTR("%@/IPv4"), linkPath);
		got  = SCPreferencesPathGetValue(prefs, deep);
		CFRelease(deep);
		if (!dict_has(got, "Method", "DHCP")) {
			fail("linked content did not persist");
			goto out;
		}
	}
	printf("  CommitChanges/reopen: link persisted\n");

	/* SetLink must reject a dangling target */
	{
		CFStringRef	bad	= mkstr("/no/such/target");
		CFStringRef	badAt	= mkstr("/bad/link");
		Boolean		ok	= SCPreferencesPathSetLink(prefs,
							badAt, bad);
		CFRelease(bad);
		CFRelease(badAt);
		if (ok) {
			fail("SetLink accepted a dangling target");
			goto out;
		}
	}

	/* a __LINK__ cycle must be caught with kSCStatusMaxLink */
	{
		CFStringRef	pa	= mkstr("/cyc/a");
		CFStringRef	pb	= mkstr("/cyc/b");
		CFStringRef	probe	= mkstr("/cyc/a/x");
		CFDictionaryRef	la	= mkdict1("__LINK__", "/cyc/b");
		CFDictionaryRef	lb	= mkdict1("__LINK__", "/cyc/a");
		Boolean		set;

		set = SCPreferencesPathSetValue(prefs, pa, la) &&
		      SCPreferencesPathSetValue(prefs, pb, lb);
		got = set ? SCPreferencesPathGetValue(prefs, probe) : NULL;
		CFRelease(pa);
		CFRelease(pb);
		CFRelease(probe);
		CFRelease(la);
		CFRelease(lb);
		if (!set) {
			fail("could not stage the link cycle");
			goto out;
		}
		/* walking the cycle must fail with the link-count error */
		if ((got != NULL) || (SCError() != kSCStatusMaxLink)) {
			fail("link cycle not caught with kSCStatusMaxLink");
			goto out;
		}
	}
	printf("  link cycle caught (kSCStatusMaxLink); dangling link rejected\n");

	printf("SC-PLINK-OK: SCPreferences path links work\n");
	rc = 0;

    out :
	fflush(stdout);
	if (prefs != NULL)	CFRelease(prefs);
	if (child != NULL)	CFRelease(child);
	if (ipv4Path != NULL)	CFRelease(ipv4Path);
	if (svc != NULL)	CFRelease(svc);
	if (ipv4 != NULL)	CFRelease(ipv4);
	CFRelease(name);
	CFRelease(prefsID);
	CFRelease(prefix);
	CFRelease(linkPath);
	return rc;
}
