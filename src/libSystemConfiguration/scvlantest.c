/*
 * scvlantest.c — libSystemConfiguration SCNetworkConfiguration iter 5
 * test client: SCVLANInterface.
 *
 * Creates an 802.1Q VLAN interface on the guest's e1000 interface,
 * checks its physical interface / tag / display name / options, commits,
 * reopens the preferences and confirms the VLAN persisted, then removes
 * it. Also confirms a duplicate physical+tag pair is rejected.
 *
 * run.sh runs it and boot-test.sh gates on the SC-VLAN-OK / -FAIL
 * marker.
 */

#include <SystemConfiguration/SystemConfiguration.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>

#define	SCT_ID	"scvlantest.plist"

static void
fail(const char *msg)
{
	printf("SC-VLAN-FAIL: %s\n", msg);
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

/* TRUE iff `vlan` looks right: a VLAN on `physBSD` with tag `tag` */
static Boolean
vlan_matches(SCVLANInterfaceRef vlan, CFStringRef physBSD, CFNumberRef tag)
{
	SCNetworkInterfaceRef	phys = SCVLANInterfaceGetPhysicalInterface(vlan);
	CFNumberRef		t    = SCVLANInterfaceGetTag(vlan);

	return ((phys != NULL) && (t != NULL) &&
		CFEqual(SCNetworkInterfaceGetBSDName(phys), physBSD) &&
		CFEqual(t, tag));
}

int
main(void)
{
	SCPreferencesRef	prefs	= NULL;
	CFArrayRef		allif	= NULL;
	CFArrayRef		vlans	= NULL;
	SCNetworkInterfaceRef	physical;
	SCVLANInterfaceRef	vlan	= NULL;
	SCVLANInterfaceRef	vlan2	= NULL;
	CFNumberRef		tag	= NULL;
	CFMutableDictionaryRef	opts	= NULL;
	CFStringRef		origBSD	= NULL;
	CFStringRef		name, prefsID, wantName, mtuKey, mtuVal;
	int			tagVal	= 100;
	int			rc	= 1;

	printf("scvlantest: SCVLANInterface\n");
	fflush(stdout);

	name	 = mkstr("scvlantest");
	prefsID	 = mkstr(SCT_ID);
	wantName = mkstr("My VLAN");
	mtuKey	 = mkstr("mtu");
	mtuVal	 = mkstr("1500");
	tag	 = CFNumberCreate(NULL, kCFNumberIntType, &tagVal);

	prefs = SCPreferencesCreate(NULL, name, prefsID);
	if (prefs == NULL) {
		fail("SCPreferencesCreate failed");
		goto out;
	}

	allif    = SCNetworkInterfaceCopyAll();
	physical = first_ethernet(allif);
	if (physical == NULL) {
		fail("no Ethernet interface for a VLAN");
		goto out;
	}
	origBSD = CFStringCreateCopy(NULL, SCNetworkInterfaceGetBSDName(physical));

	/* no VLANs to start */
	vlans = SCVLANInterfaceCopyAll(prefs);
	if ((vlans == NULL) || (CFArrayGetCount(vlans) != 0)) {
		fail("a fresh preferences has VLANs");
		goto out;
	}
	CFRelease(vlans);
	vlans = NULL;

	/* create a VLAN */
	vlan = SCVLANInterfaceCreate(prefs, physical, tag);
	if (vlan == NULL) {
		fail("SCVLANInterfaceCreate failed");
		goto out;
	}
	if (!CFEqual(SCNetworkInterfaceGetInterfaceType(vlan),
		     kSCNetworkInterfaceTypeVLAN)) {
		fail("created interface is not of type VLAN");
		goto out;
	}
	if (!CFStringHasPrefix(SCNetworkInterfaceGetBSDName(vlan),
			       CFSTR("vlan"))) {
		fail("VLAN interface has no vlanN BSD name");
		goto out;
	}
	if (!vlan_matches(vlan, origBSD, tag)) {
		fail("VLAN physical interface / tag wrong");
		goto out;
	}
	printf("  VLANInterfaceCreate: VLAN created on the interface\n");

	/* a duplicate physical + tag pair is rejected */
	{
		SCVLANInterfaceRef	dup;

		dup = SCVLANInterfaceCreate(prefs, physical, tag);
		if ((dup != NULL) || (SCError() != kSCStatusKeyExists)) {
			if (dup != NULL) CFRelease(dup);
			fail("a duplicate VLAN was allowed");
			goto out;
		}
	}

	/* display name + options */
	if (!SCVLANInterfaceSetLocalizedDisplayName(vlan, wantName) ||
	    !CFEqual(SCNetworkInterfaceGetLocalizedDisplayName(vlan),
		     wantName)) {
		fail("VLAN display name did not round-trip");
		goto out;
	}
	opts = CFDictionaryCreateMutable(NULL, 0,
					 &kCFTypeDictionaryKeyCallBacks,
					 &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(opts, mtuKey, mtuVal);
	if (!SCVLANInterfaceSetOptions(vlan, opts)) {
		fail("SCVLANInterfaceSetOptions failed");
		goto out;
	}
	{
		CFDictionaryRef	got = SCVLANInterfaceGetOptions(vlan);
		CFTypeRef	v   = (got != NULL)
				      ? CFDictionaryGetValue(got, mtuKey) : NULL;

		if ((v == NULL) || !CFEqual(v, mtuVal)) {
			fail("VLAN options did not round-trip");
			goto out;
		}
	}
	printf("  SetDisplayName/SetOptions: VLAN configured\n");

	vlans = SCVLANInterfaceCopyAll(prefs);
	if ((vlans == NULL) || (CFArrayGetCount(vlans) != 1)) {
		fail("SCVLANInterfaceCopyAll count wrong");
		goto out;
	}
	CFRelease(vlans);
	vlans = NULL;

	/* commit, reopen, confirm the VLAN persisted */
	if (!SCPreferencesCommitChanges(prefs)) {
		fail("SCPreferencesCommitChanges failed");
		goto out;
	}
	CFRelease(vlan);
	vlan = NULL;
	CFRelease(prefs);
	prefs = SCPreferencesCreate(NULL, name, prefsID);
	if (prefs == NULL) {
		fail("SCPreferencesCreate (reopen) failed");
		goto out;
	}

	vlans = SCVLANInterfaceCopyAll(prefs);
	if ((vlans == NULL) || (CFArrayGetCount(vlans) != 1)) {
		fail("VLAN did not persist");
		goto out;
	}
	vlan2 = (SCVLANInterfaceRef)CFArrayGetValueAtIndex(vlans, 0);
	CFRetain(vlan2);
	if (!vlan_matches(vlan2, origBSD, tag)) {
		fail("persisted VLAN physical / tag wrong");
		goto out;
	}
	if (!CFEqual(SCNetworkInterfaceGetLocalizedDisplayName(vlan2),
		     wantName)) {
		fail("VLAN display name did not persist");
		goto out;
	}
	{
		CFDictionaryRef	got = SCVLANInterfaceGetOptions(vlan2);
		CFTypeRef	v   = (got != NULL)
				      ? CFDictionaryGetValue(got, mtuKey) : NULL;

		if ((v == NULL) || !CFEqual(v, mtuVal)) {
			fail("VLAN options did not persist");
			goto out;
		}
	}
	printf("  CommitChanges/reopen: VLAN persisted\n");

	/* remove it */
	if (!SCVLANInterfaceRemove(vlan2)) {
		fail("SCVLANInterfaceRemove failed");
		goto out;
	}
	CFRelease(vlans);
	vlans = SCVLANInterfaceCopyAll(prefs);
	if ((vlans == NULL) || (CFArrayGetCount(vlans) != 0)) {
		fail("VLAN still present after removal");
		goto out;
	}
	printf("  VLANInterfaceRemove: VLAN removed\n");

	printf("SC-VLAN-OK: SCVLANInterface works\n");
	rc = 0;

    out :
	fflush(stdout);
	if (vlans != NULL)	CFRelease(vlans);
	if (vlan != NULL)	CFRelease(vlan);
	if (vlan2 != NULL)	CFRelease(vlan2);
	if (opts != NULL)	CFRelease(opts);
	if (allif != NULL)	CFRelease(allif);
	if (prefs != NULL)	CFRelease(prefs);
	if (tag != NULL)	CFRelease(tag);
	if (origBSD != NULL)	CFRelease(origBSD);
	CFRelease(name);
	CFRelease(prefsID);
	CFRelease(wantName);
	CFRelease(mtuKey);
	CFRelease(mtuVal);
	return rc;
}
