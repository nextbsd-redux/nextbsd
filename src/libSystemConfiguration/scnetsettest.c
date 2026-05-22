/*
 * scnetsettest.c — libSystemConfiguration SCNetworkConfiguration iter 4
 * test client: SCNetworkSet.
 *
 * Creates a network set ("location"), names it, adds a network service
 * to it (and removes / re-adds it), sets a service order, makes the set
 * current, commits, reopens the preferences and confirms the set, its
 * name, its members, its order and the current-set selection all
 * persisted — and finally confirms the active set cannot be removed but
 * an inactive one can.
 *
 * run.sh runs it and boot-test.sh gates on the SC-NETSET-OK / -FAIL
 * marker.
 */

#include <SystemConfiguration/SystemConfiguration.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>

#define	SCT_ID	"scnetsettest.plist"

static void
fail(const char *msg)
{
	printf("SC-NETSET-FAIL: %s\n", msg);
	fflush(stdout);
}

static CFStringRef
mkstr(const char *s)
{
	return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
}

/* the first Ethernet interface from SCNetworkInterfaceCopyAll, or NULL */
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

/* TRUE iff `services` holds a service whose ID equals `serviceID` */
static Boolean
services_have(CFArrayRef services, CFStringRef serviceID)
{
	CFIndex	i, n;

	n = (services != NULL) ? CFArrayGetCount(services) : 0;
	for (i = 0; i < n; i++) {
		SCNetworkServiceRef	svc;

		svc = (SCNetworkServiceRef)CFArrayGetValueAtIndex(services, i);
		if (CFEqual(SCNetworkServiceGetServiceID(svc), serviceID)) {
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
	SCNetworkInterfaceRef	iface;
	SCNetworkServiceRef	svc	= NULL;
	SCNetworkSetRef		set	= NULL;
	SCNetworkSetRef		set2	= NULL;
	SCNetworkSetRef		setB	= NULL;
	SCNetworkSetRef		cur	= NULL;
	CFArrayRef		services = NULL;
	CFArrayRef		allsets	= NULL;
	CFMutableArrayRef	order	= NULL;
	CFStringRef		svcID	= NULL;
	CFStringRef		setID	= NULL;
	CFStringRef		name, prefsID, wantName;
	int			rc	= 1;

	printf("scnetsettest: SCNetworkSet\n");
	fflush(stdout);

	name	 = mkstr("scnetsettest");
	prefsID	 = mkstr(SCT_ID);
	wantName = mkstr("Test Location");

	prefs = SCPreferencesCreate(NULL, name, prefsID);
	if (prefs == NULL) {
		fail("SCPreferencesCreate failed");
		goto out;
	}

	/* a service to populate the set with */
	allif = SCNetworkInterfaceCopyAll();
	iface = first_ethernet(allif);
	if (iface == NULL) {
		fail("no Ethernet interface for a service");
		goto out;
	}
	svc = SCNetworkServiceCreate(prefs, iface);
	if (svc == NULL) {
		fail("SCNetworkServiceCreate failed");
		goto out;
	}
	svcID = CFStringCreateCopy(NULL, SCNetworkServiceGetServiceID(svc));

	/* create + name the set */
	set = SCNetworkSetCreate(prefs);
	if (set == NULL) {
		fail("SCNetworkSetCreate failed");
		goto out;
	}
	setID = CFStringCreateCopy(NULL, SCNetworkSetGetSetID(set));
	if (SCNetworkSetGetName(set) != NULL) {
		fail("a new set already has a name");
		goto out;
	}
	if (!SCNetworkSetSetName(set, wantName) ||
	    !CFEqual(SCNetworkSetGetName(set), wantName)) {
		fail("SCNetworkSetSetName did not round-trip");
		goto out;
	}
	printf("  SetCreate/SetName: set named\n");

	/* a new set has no services */
	services = SCNetworkSetCopyServices(set);
	if ((services == NULL) || (CFArrayGetCount(services) != 0)) {
		fail("a new set is not empty");
		goto out;
	}
	CFRelease(services);
	services = NULL;

	/* add, confirm, remove, re-add the service */
	if (!SCNetworkSetAddService(set, svc)) {
		fail("SCNetworkSetAddService failed");
		goto out;
	}
	services = SCNetworkSetCopyServices(set);
	if ((CFArrayGetCount(services) != 1) ||
	    !services_have(services, svcID)) {
		fail("set does not contain the added service");
		goto out;
	}
	CFRelease(services);
	services = NULL;

	if (!SCNetworkSetRemoveService(set, svc)) {
		fail("SCNetworkSetRemoveService failed");
		goto out;
	}
	services = SCNetworkSetCopyServices(set);
	if (CFArrayGetCount(services) != 0) {
		fail("service still present after removal");
		goto out;
	}
	CFRelease(services);
	services = NULL;

	if (!SCNetworkSetAddService(set, svc)) {
		fail("SCNetworkSetAddService (re-add) failed");
		goto out;
	}
	printf("  Add/RemoveService: membership works\n");

	/* a service order */
	order = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	CFArrayAppendValue(order, svcID);
	if (!SCNetworkSetSetServiceOrder(set, order)) {
		fail("SCNetworkSetSetServiceOrder failed");
		goto out;
	}
	{
		CFArrayRef	got = SCNetworkSetGetServiceOrder(set);

		if ((got == NULL) || (CFArrayGetCount(got) != 1) ||
		    !CFEqual(CFArrayGetValueAtIndex(got, 0), svcID)) {
			fail("service order did not round-trip");
			goto out;
		}
	}

	/* make the set current */
	if (!SCNetworkSetSetCurrent(set)) {
		fail("SCNetworkSetSetCurrent failed");
		goto out;
	}
	cur = SCNetworkSetCopyCurrent(prefs);
	if ((cur == NULL) ||
	    !CFEqual(SCNetworkSetGetSetID(cur), setID)) {
		fail("SCNetworkSetCopyCurrent mismatch");
		goto out;
	}
	CFRelease(cur);
	cur = NULL;
	printf("  ServiceOrder/SetCurrent: order + current set work\n");

	/* commit, reopen, confirm everything persisted */
	if (!SCPreferencesCommitChanges(prefs)) {
		fail("SCPreferencesCommitChanges failed");
		goto out;
	}
	CFRelease(set);
	set = NULL;
	CFRelease(prefs);
	prefs = SCPreferencesCreate(NULL, name, prefsID);
	if (prefs == NULL) {
		fail("SCPreferencesCreate (reopen) failed");
		goto out;
	}

	set2 = SCNetworkSetCopy(prefs, setID);
	if (set2 == NULL) {
		fail("SCNetworkSetCopy after reopen failed");
		goto out;
	}
	if (!CFEqual(SCNetworkSetGetName(set2), wantName)) {
		fail("set name did not persist");
		goto out;
	}
	services = SCNetworkSetCopyServices(set2);
	if ((CFArrayGetCount(services) != 1) ||
	    !services_have(services, svcID)) {
		fail("set membership did not persist");
		goto out;
	}
	CFRelease(services);
	services = NULL;
	{
		CFArrayRef	got = SCNetworkSetGetServiceOrder(set2);

		if ((got == NULL) || (CFArrayGetCount(got) != 1)) {
			fail("service order did not persist");
			goto out;
		}
	}
	cur = SCNetworkSetCopyCurrent(prefs);
	if ((cur == NULL) ||
	    !CFEqual(SCNetworkSetGetSetID(cur), setID)) {
		fail("current set did not persist");
		goto out;
	}
	CFRelease(cur);
	cur = NULL;
	allsets = SCNetworkSetCopyAll(prefs);
	if ((allsets == NULL) || (CFArrayGetCount(allsets) < 1)) {
		fail("SCNetworkSetCopyAll did not list the set");
		goto out;
	}
	printf("  CommitChanges/reopen: set persisted\n");

	/* the active set cannot be removed */
	if (SCNetworkSetRemove(set2)) {
		fail("the active set was removed");
		goto out;
	}
	if (SCError() != kSCStatusInvalidArgument) {
		fail("removing the active set gave the wrong error");
		goto out;
	}
	/* make another set current, then the first is removable */
	setB = SCNetworkSetCreate(prefs);
	if ((setB == NULL) || !SCNetworkSetSetCurrent(setB)) {
		fail("could not create + activate a second set");
		goto out;
	}
	if (!SCNetworkSetRemove(set2)) {
		fail("SCNetworkSetRemove failed for an inactive set");
		goto out;
	}
	if (SCNetworkSetCopy(prefs, setID) != NULL) {
		fail("set still present after removal");
		goto out;
	}
	printf("  SetRemove: active set protected, inactive removed\n");

	printf("SC-NETSET-OK: SCNetworkSet works\n");
	rc = 0;

    out :
	fflush(stdout);
	if (services != NULL)	CFRelease(services);
	if (allsets != NULL)	CFRelease(allsets);
	if (order != NULL)	CFRelease(order);
	if (cur != NULL)	CFRelease(cur);
	if (set != NULL)	CFRelease(set);
	if (set2 != NULL)	CFRelease(set2);
	if (setB != NULL)	CFRelease(setB);
	if (svc != NULL)	CFRelease(svc);
	if (allif != NULL)	CFRelease(allif);
	if (prefs != NULL)	CFRelease(prefs);
	if (svcID != NULL)	CFRelease(svcID);
	if (setID != NULL)	CFRelease(setID);
	CFRelease(name);
	CFRelease(prefsID);
	CFRelease(wantName);
	return rc;
}
