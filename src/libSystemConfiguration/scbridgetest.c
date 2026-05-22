/*
 * scbridgetest.c — libSystemConfiguration SCNetworkConfiguration iter 7
 * test client: SCBridgeInterface.
 *
 * Creates a bridge interface, adds the guest's e1000 interface as a
 * member, checks the member list / display name / options / the
 * AllowConfiguredMembers flag and the available-member query, commits,
 * reopens the preferences to confirm the bridge persisted, then removes
 * it.
 *
 * run.sh runs it and boot-test.sh gates on the SC-BRIDGE-OK / -FAIL
 * marker.
 */

#include <SystemConfiguration/SystemConfiguration.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>

#define	SCT_ID	"scbridgetest.plist"

static void
fail(const char *msg)
{
	printf("SC-BRIDGE-FAIL: %s\n", msg);
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
	CFArrayRef		bridges	= NULL;
	CFArrayRef		avail	= NULL;
	CFMutableArrayRef	members	= NULL;
	CFMutableDictionaryRef	opts	= NULL;
	SCNetworkInterfaceRef	member;
	SCBridgeInterfaceRef	bridge	= NULL;
	SCBridgeInterfaceRef	bridge2	= NULL;
	CFStringRef		memberBSD = NULL;
	CFStringRef		name, prefsID, wantName, fooKey, barVal;
	int			rc	= 1;

	printf("scbridgetest: SCBridgeInterface\n");
	fflush(stdout);

	name	 = mkstr("scbridgetest");
	prefsID	 = mkstr(SCT_ID);
	wantName = mkstr("My Bridge");
	fooKey	 = mkstr("foo");
	barVal	 = mkstr("bar");

	prefs = SCPreferencesCreate(NULL, name, prefsID);
	if (prefs == NULL) {
		fail("SCPreferencesCreate failed");
		goto out;
	}

	allif  = SCNetworkInterfaceCopyAll();
	member = first_ethernet(allif);
	if (member == NULL) {
		fail("no Ethernet interface for a bridge member");
		goto out;
	}
	memberBSD = CFStringCreateCopy(NULL, SCNetworkInterfaceGetBSDName(member));

	/* no bridges to start */
	bridges = SCBridgeInterfaceCopyAll(prefs);
	if ((bridges == NULL) || (CFArrayGetCount(bridges) != 0)) {
		fail("a fresh preferences has bridges");
		goto out;
	}
	CFRelease(bridges);
	bridges = NULL;

	/* create an (empty) bridge */
	bridge = SCBridgeInterfaceCreate(prefs);
	if (bridge == NULL) {
		fail("SCBridgeInterfaceCreate failed");
		goto out;
	}
	if (!CFEqual(SCNetworkInterfaceGetInterfaceType(bridge),
		     kSCNetworkInterfaceTypeBridge)) {
		fail("created interface is not of type Bridge");
		goto out;
	}
	if (!CFStringHasPrefix(SCNetworkInterfaceGetBSDName(bridge),
			       CFSTR("bridge"))) {
		fail("Bridge interface has no bridgeN BSD name");
		goto out;
	}
	{
		CFArrayRef	m0 = SCBridgeInterfaceGetMemberInterfaces(bridge);

		if ((m0 == NULL) || (CFArrayGetCount(m0) != 0)) {
			fail("a new bridge is not empty");
			goto out;
		}
	}

	/* the e1000 interface is available before it is bridged */
	avail = SCBridgeInterfaceCopyAvailableMemberInterfaces(prefs);
	if (!array_has_bsd(avail, memberBSD)) {
		fail("interface not listed as an available bridge member");
		goto out;
	}
	CFRelease(avail);
	avail = NULL;
	printf("  BridgeInterfaceCreate: empty bridge created\n");

	/* add the interface as a member */
	members = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	CFArrayAppendValue(members, member);
	if (!SCBridgeInterfaceSetMemberInterfaces(bridge, members)) {
		fail("SCBridgeInterfaceSetMemberInterfaces failed");
		goto out;
	}
	{
		CFArrayRef	got = SCBridgeInterfaceGetMemberInterfaces(bridge);

		if ((got == NULL) || (CFArrayGetCount(got) != 1) ||
		    !array_has_bsd(got, memberBSD)) {
			fail("bridge member list did not round-trip");
			goto out;
		}
	}

	/* once bridged, the interface is no longer available */
	avail = SCBridgeInterfaceCopyAvailableMemberInterfaces(prefs);
	if (array_has_bsd(avail, memberBSD)) {
		fail("a bridged interface is still listed as available");
		goto out;
	}
	CFRelease(avail);
	avail = NULL;
	printf("  SetMemberInterfaces: member added, availability updated\n");

	/* display name, options, and the AllowConfiguredMembers flag */
	if (!SCBridgeInterfaceSetLocalizedDisplayName(bridge, wantName) ||
	    !CFEqual(SCNetworkInterfaceGetLocalizedDisplayName(bridge),
		     wantName)) {
		fail("bridge display name did not round-trip");
		goto out;
	}
	opts = CFDictionaryCreateMutable(NULL, 0,
					 &kCFTypeDictionaryKeyCallBacks,
					 &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(opts, fooKey, barVal);
	if (!SCBridgeInterfaceSetOptions(bridge, opts)) {
		fail("SCBridgeInterfaceSetOptions failed");
		goto out;
	}
	if (SCBridgeInterfaceGetAllowConfiguredMembers(bridge)) {
		fail("AllowConfiguredMembers is set on a new bridge");
		goto out;
	}
	if (!SCBridgeInterfaceSetAllowConfiguredMembers(bridge, TRUE) ||
	    !SCBridgeInterfaceGetAllowConfiguredMembers(bridge)) {
		fail("SCBridgeInterfaceSetAllowConfiguredMembers did not take");
		goto out;
	}
	{
		/* setting the flag must not lose the existing options */
		CFDictionaryRef	got = SCBridgeInterfaceGetOptions(bridge);
		CFTypeRef	v   = (got != NULL)
				      ? CFDictionaryGetValue(got, fooKey) : NULL;

		if ((v == NULL) || !CFEqual(v, barVal)) {
			fail("bridge options did not round-trip");
			goto out;
		}
	}
	printf("  SetOptions/AllowConfiguredMembers: bridge configured\n");

	bridges = SCBridgeInterfaceCopyAll(prefs);
	if ((bridges == NULL) || (CFArrayGetCount(bridges) != 1)) {
		fail("SCBridgeInterfaceCopyAll count wrong");
		goto out;
	}
	CFRelease(bridges);
	bridges = NULL;

	/* commit, reopen, confirm the bridge persisted */
	if (!SCPreferencesCommitChanges(prefs)) {
		fail("SCPreferencesCommitChanges failed");
		goto out;
	}
	CFRelease(bridge);
	bridge = NULL;
	CFRelease(prefs);
	prefs = SCPreferencesCreate(NULL, name, prefsID);
	if (prefs == NULL) {
		fail("SCPreferencesCreate (reopen) failed");
		goto out;
	}

	bridges = SCBridgeInterfaceCopyAll(prefs);
	if ((bridges == NULL) || (CFArrayGetCount(bridges) != 1)) {
		fail("bridge did not persist");
		goto out;
	}
	bridge2 = (SCBridgeInterfaceRef)CFArrayGetValueAtIndex(bridges, 0);
	CFRetain(bridge2);
	{
		CFArrayRef	got = SCBridgeInterfaceGetMemberInterfaces(bridge2);

		if ((got == NULL) || (CFArrayGetCount(got) != 1) ||
		    !array_has_bsd(got, memberBSD)) {
			fail("bridge members did not persist");
			goto out;
		}
	}
	if (!CFEqual(SCNetworkInterfaceGetLocalizedDisplayName(bridge2),
		     wantName)) {
		fail("bridge display name did not persist");
		goto out;
	}
	if (!SCBridgeInterfaceGetAllowConfiguredMembers(bridge2)) {
		fail("AllowConfiguredMembers did not persist");
		goto out;
	}
	{
		CFDictionaryRef	got = SCBridgeInterfaceGetOptions(bridge2);
		CFTypeRef	v   = (got != NULL)
				      ? CFDictionaryGetValue(got, fooKey) : NULL;

		if ((v == NULL) || !CFEqual(v, barVal)) {
			fail("bridge options did not persist");
			goto out;
		}
	}
	printf("  CommitChanges/reopen: bridge persisted\n");

	/* remove it */
	if (!SCBridgeInterfaceRemove(bridge2)) {
		fail("SCBridgeInterfaceRemove failed");
		goto out;
	}
	CFRelease(bridges);
	bridges = SCBridgeInterfaceCopyAll(prefs);
	if ((bridges == NULL) || (CFArrayGetCount(bridges) != 0)) {
		fail("bridge still present after removal");
		goto out;
	}
	printf("  BridgeInterfaceRemove: bridge removed\n");

	printf("SC-BRIDGE-OK: SCBridgeInterface works\n");
	rc = 0;

    out :
	fflush(stdout);
	if (bridges != NULL)	CFRelease(bridges);
	if (avail != NULL)	CFRelease(avail);
	if (members != NULL)	CFRelease(members);
	if (opts != NULL)	CFRelease(opts);
	if (bridge != NULL)	CFRelease(bridge);
	if (bridge2 != NULL)	CFRelease(bridge2);
	if (allif != NULL)	CFRelease(allif);
	if (prefs != NULL)	CFRelease(prefs);
	if (memberBSD != NULL)	CFRelease(memberBSD);
	CFRelease(name);
	CFRelease(prefsID);
	CFRelease(wantName);
	CFRelease(fooKey);
	CFRelease(barVal);
	return rc;
}
