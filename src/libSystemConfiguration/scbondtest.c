/*
 * scbondtest.c — libSystemConfiguration SCNetworkConfiguration iter 6
 * test client: SCBondInterface.
 *
 * Creates a bond (link-aggregation) interface, adds the guest's e1000
 * interface as a member, checks the member list / display name /
 * options and the available-member query, commits, reopens the
 * preferences to confirm the bond persisted, then removes it.
 *
 * run.sh runs it and boot-test.sh gates on the SC-BOND-OK / -FAIL
 * marker.
 */

#include <SystemConfiguration/SystemConfiguration.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>

#define	SCT_ID	"scbondtest.plist"

static void
fail(const char *msg)
{
	printf("SC-BOND-FAIL: %s\n", msg);
	fflush(stdout);
}

static CFStringRef
mkstr(const char *s)
{
	return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
}

static SCNetworkInterfaceRef
first_ethernet(CFArrayRef all)
{
	CFIndex	i, n;

	n = (all != NULL) ? CFArrayGetCount(all) : 0;
	for (i = 0; i < n; i++) {
		SCNetworkInterfaceRef	iface;

		iface = (SCNetworkInterfaceRef)CFArrayGetValueAtIndex(all, i);
		if (CFEqual(SCNetworkInterfaceGetInterfaceType(iface),
			    kSCNetworkInterfaceTypeEthernet)) {
			return iface;
		}
	}
	return NULL;
}

/* TRUE iff `array` (of SCNetworkInterfaceRef) holds an iface named `bsd` */
static Boolean
array_has_bsd(CFArrayRef array, CFStringRef bsd)
{
	CFIndex	i, n;

	n = (array != NULL) ? CFArrayGetCount(array) : 0;
	for (i = 0; i < n; i++) {
		SCNetworkInterfaceRef	iface;
		CFStringRef		name;

		iface = (SCNetworkInterfaceRef)CFArrayGetValueAtIndex(array, i);
		name  = SCNetworkInterfaceGetBSDName(iface);
		if ((name != NULL) && CFEqual(name, bsd)) {
			return TRUE;
		}
	}
	return FALSE;
}

int
main(void)
{
	SCPreferencesRef	prefs	= NULL;
	CFArrayRef		allif	= NULL;
	CFArrayRef		bonds	= NULL;
	CFArrayRef		avail	= NULL;
	CFMutableArrayRef	members	= NULL;
	CFMutableDictionaryRef	opts	= NULL;
	SCNetworkInterfaceRef	member;
	SCBondInterfaceRef	bond	= NULL;
	SCBondInterfaceRef	bond2	= NULL;
	CFStringRef		memberBSD = NULL;
	CFStringRef		name, prefsID, wantName, modeKey, modeVal;
	int			rc	= 1;

	printf("scbondtest: SCBondInterface\n");
	fflush(stdout);

	name	 = mkstr("scbondtest");
	prefsID	 = mkstr(SCT_ID);
	wantName = mkstr("My Bond");
	modeKey	 = mkstr("mode");
	modeVal	 = mkstr("lacp");

	prefs = SCPreferencesCreate(NULL, name, prefsID);
	if (prefs == NULL) {
		fail("SCPreferencesCreate failed");
		goto out;
	}

	allif  = SCNetworkInterfaceCopyAll();
	member = first_ethernet(allif);
	if (member == NULL) {
		fail("no Ethernet interface for a bond member");
		goto out;
	}
	memberBSD = CFStringCreateCopy(NULL, SCNetworkInterfaceGetBSDName(member));

	/* no bonds to start */
	bonds = SCBondInterfaceCopyAll(prefs);
	if ((bonds == NULL) || (CFArrayGetCount(bonds) != 0)) {
		fail("a fresh preferences has bonds");
		goto out;
	}
	CFRelease(bonds);
	bonds = NULL;

	/* create an (empty) bond */
	bond = SCBondInterfaceCreate(prefs);
	if (bond == NULL) {
		fail("SCBondInterfaceCreate failed");
		goto out;
	}
	if (!CFEqual(SCNetworkInterfaceGetInterfaceType(bond),
		     kSCNetworkInterfaceTypeBond)) {
		fail("created interface is not of type Bond");
		goto out;
	}
	if (!CFStringHasPrefix(SCNetworkInterfaceGetBSDName(bond),
			       CFSTR("bond"))) {
		fail("Bond interface has no bondN BSD name");
		goto out;
	}
	{
		CFArrayRef	m0 = SCBondInterfaceGetMemberInterfaces(bond);

		if ((m0 == NULL) || (CFArrayGetCount(m0) != 0)) {
			fail("a new bond is not empty");
			goto out;
		}
	}

	/* the e1000 interface is available before it is bonded */
	avail = SCBondInterfaceCopyAvailableMemberInterfaces(prefs);
	if (!array_has_bsd(avail, memberBSD)) {
		fail("interface not listed as an available bond member");
		goto out;
	}
	CFRelease(avail);
	avail = NULL;
	printf("  BondInterfaceCreate: empty bond created\n");

	/* add the interface as a member */
	members = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	CFArrayAppendValue(members, member);
	if (!SCBondInterfaceSetMemberInterfaces(bond, members)) {
		fail("SCBondInterfaceSetMemberInterfaces failed");
		goto out;
	}
	{
		CFArrayRef	got = SCBondInterfaceGetMemberInterfaces(bond);

		if ((got == NULL) || (CFArrayGetCount(got) != 1) ||
		    !array_has_bsd(got, memberBSD)) {
			fail("bond member list did not round-trip");
			goto out;
		}
	}

	/* once bonded, the interface is no longer available */
	avail = SCBondInterfaceCopyAvailableMemberInterfaces(prefs);
	if (array_has_bsd(avail, memberBSD)) {
		fail("a bonded interface is still listed as available");
		goto out;
	}
	CFRelease(avail);
	avail = NULL;
	printf("  SetMemberInterfaces: member added, availability updated\n");

	/* display name + options */
	if (!SCBondInterfaceSetLocalizedDisplayName(bond, wantName) ||
	    !CFEqual(SCNetworkInterfaceGetLocalizedDisplayName(bond),
		     wantName)) {
		fail("bond display name did not round-trip");
		goto out;
	}
	opts = CFDictionaryCreateMutable(NULL, 0,
					 &kCFTypeDictionaryKeyCallBacks,
					 &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(opts, modeKey, modeVal);
	if (!SCBondInterfaceSetOptions(bond, opts)) {
		fail("SCBondInterfaceSetOptions failed");
		goto out;
	}
	{
		CFDictionaryRef	got = SCBondInterfaceGetOptions(bond);
		CFTypeRef	v   = (got != NULL)
				      ? CFDictionaryGetValue(got, modeKey) : NULL;

		if ((v == NULL) || !CFEqual(v, modeVal)) {
			fail("bond options did not round-trip");
			goto out;
		}
	}
	printf("  SetDisplayName/SetOptions: bond configured\n");

	bonds = SCBondInterfaceCopyAll(prefs);
	if ((bonds == NULL) || (CFArrayGetCount(bonds) != 1)) {
		fail("SCBondInterfaceCopyAll count wrong");
		goto out;
	}
	CFRelease(bonds);
	bonds = NULL;

	/* commit, reopen, confirm the bond persisted */
	if (!SCPreferencesCommitChanges(prefs)) {
		fail("SCPreferencesCommitChanges failed");
		goto out;
	}
	CFRelease(bond);
	bond = NULL;
	CFRelease(prefs);
	prefs = SCPreferencesCreate(NULL, name, prefsID);
	if (prefs == NULL) {
		fail("SCPreferencesCreate (reopen) failed");
		goto out;
	}

	bonds = SCBondInterfaceCopyAll(prefs);
	if ((bonds == NULL) || (CFArrayGetCount(bonds) != 1)) {
		fail("bond did not persist");
		goto out;
	}
	bond2 = (SCBondInterfaceRef)CFArrayGetValueAtIndex(bonds, 0);
	CFRetain(bond2);
	{
		CFArrayRef	got = SCBondInterfaceGetMemberInterfaces(bond2);

		if ((got == NULL) || (CFArrayGetCount(got) != 1) ||
		    !array_has_bsd(got, memberBSD)) {
			fail("bond members did not persist");
			goto out;
		}
	}
	if (!CFEqual(SCNetworkInterfaceGetLocalizedDisplayName(bond2),
		     wantName)) {
		fail("bond display name did not persist");
		goto out;
	}
	{
		CFDictionaryRef	got = SCBondInterfaceGetOptions(bond2);
		CFTypeRef	v   = (got != NULL)
				      ? CFDictionaryGetValue(got, modeKey) : NULL;

		if ((v == NULL) || !CFEqual(v, modeVal)) {
			fail("bond options did not persist");
			goto out;
		}
	}
	printf("  CommitChanges/reopen: bond persisted\n");

	/* remove it */
	if (!SCBondInterfaceRemove(bond2)) {
		fail("SCBondInterfaceRemove failed");
		goto out;
	}
	CFRelease(bonds);
	bonds = SCBondInterfaceCopyAll(prefs);
	if ((bonds == NULL) || (CFArrayGetCount(bonds) != 0)) {
		fail("bond still present after removal");
		goto out;
	}
	printf("  BondInterfaceRemove: bond removed\n");

	printf("SC-BOND-OK: SCBondInterface works\n");
	rc = 0;

    out :
	fflush(stdout);
	if (bonds != NULL)	CFRelease(bonds);
	if (avail != NULL)	CFRelease(avail);
	if (members != NULL)	CFRelease(members);
	if (opts != NULL)	CFRelease(opts);
	if (bond != NULL)	CFRelease(bond);
	if (bond2 != NULL)	CFRelease(bond2);
	if (allif != NULL)	CFRelease(allif);
	if (prefs != NULL)	CFRelease(prefs);
	if (memberBSD != NULL)	CFRelease(memberBSD);
	CFRelease(name);
	CFRelease(prefsID);
	CFRelease(wantName);
	CFRelease(modeKey);
	CFRelease(modeVal);
	return rc;
}
