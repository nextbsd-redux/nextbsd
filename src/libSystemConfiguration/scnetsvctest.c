/*
 * scnetsvctest.c — libSystemConfiguration SCNetworkConfiguration iter 3
 * test client: SCNetworkService + SCNetworkProtocol.
 *
 * Creates a network service bound to the guest's e1000 interface,
 * names it, attaches an IPv4 protocol and configures it, commits, then
 * reopens the preferences and confirms the service, its name, its
 * interface and its protocol configuration all persisted — and finally
 * removes the protocol and the service.
 *
 * run.sh runs it and boot-test.sh gates on the SC-NETSVC-OK / -FAIL
 * marker.
 */

#include <SystemConfiguration/SystemConfiguration.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>

#define	SCT_ID	"scnetsvctest.plist"

static void
fail(const char *msg)
{
	printf("SC-NETSVC-FAIL: %s\n", msg);
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

	n = CFArrayGetCount(all);
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

int
main(void)
{
	SCPreferencesRef	prefs	= NULL;
	CFArrayRef		all	= NULL;
	CFArrayRef		protos	= NULL;
	CFArrayRef		all2	= NULL;
	SCNetworkInterfaceRef	iface;
	SCNetworkServiceRef	svc	= NULL;
	SCNetworkServiceRef	svc2	= NULL;
	SCNetworkProtocolRef	proto	= NULL;
	SCNetworkProtocolRef	proto2	= NULL;
	CFMutableDictionaryRef	config	= NULL;
	CFStringRef		name, prefsID, wantName;
	CFStringRef		savedID	= NULL;
	CFStringRef		origBSD	= NULL;
	CFStringRef		method, dhcp;
	int			rc	= 1;

	printf("scnetsvctest: SCNetworkService + SCNetworkProtocol\n");
	fflush(stdout);

	name	 = mkstr("scnetsvctest");
	prefsID	 = mkstr(SCT_ID);
	wantName = mkstr("Test Service");
	method	 = mkstr("Method");
	dhcp	 = mkstr("DHCP");

	prefs = SCPreferencesCreate(NULL, name, prefsID);
	if (prefs == NULL) {
		fail("SCPreferencesCreate failed");
		goto out;
	}

	all = SCNetworkInterfaceCopyAll();
	iface = (all != NULL) ? first_ethernet(all) : NULL;
	if (iface == NULL) {
		fail("no Ethernet interface to build a service on");
		goto out;
	}
	origBSD = CFStringCreateCopy(NULL, SCNetworkInterfaceGetBSDName(iface));

	/* create a service bound to the interface */
	svc = SCNetworkServiceCreate(prefs, iface);
	if (svc == NULL) {
		fail("SCNetworkServiceCreate failed");
		goto out;
	}
	savedID = CFStringCreateCopy(NULL, SCNetworkServiceGetServiceID(svc));
	if (savedID == NULL) {
		fail("SCNetworkServiceGetServiceID returned NULL");
		goto out;
	}

	/* a fresh service is named after its interface */
	if (SCNetworkServiceGetName(svc) == NULL) {
		fail("a new service has no default name");
		goto out;
	}
	if (!SCNetworkServiceSetName(svc, wantName) ||
	    !CFEqual(SCNetworkServiceGetName(svc), wantName)) {
		fail("SCNetworkServiceSetName did not round-trip");
		goto out;
	}
	printf("  ServiceCreate/SetName: service named\n");

	/* the service reports the interface it was created on */
	iface = SCNetworkServiceGetInterface(svc);
	if ((iface == NULL) ||
	    !CFEqual(SCNetworkInterfaceGetBSDName(iface), origBSD)) {
		fail("SCNetworkServiceGetInterface mismatch");
		goto out;
	}

	/* attach an IPv4 protocol */
	if (!SCNetworkServiceAddProtocolType(svc, kSCNetworkProtocolTypeIPv4)) {
		fail("SCNetworkServiceAddProtocolType(IPv4) failed");
		goto out;
	}
	if (SCNetworkServiceAddProtocolType(svc, kSCNetworkProtocolTypeIPv4)) {
		fail("adding a duplicate protocol type was allowed");
		goto out;
	}

	proto = SCNetworkServiceCopyProtocol(svc, kSCNetworkProtocolTypeIPv4);
	if ((proto == NULL) ||
	    !CFEqual(SCNetworkProtocolGetProtocolType(proto),
		     kSCNetworkProtocolTypeIPv4)) {
		fail("SCNetworkServiceCopyProtocol(IPv4) failed");
		goto out;
	}

	/* configure + enable/disable the protocol */
	config = CFDictionaryCreateMutable(NULL, 0,
					   &kCFTypeDictionaryKeyCallBacks,
					   &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(config, method, dhcp);
	if (!SCNetworkProtocolSetConfiguration(proto, config)) {
		fail("SCNetworkProtocolSetConfiguration failed");
		goto out;
	}
	{
		CFDictionaryRef	got = SCNetworkProtocolGetConfiguration(proto);
		CFTypeRef	v   = (got != NULL)
				      ? CFDictionaryGetValue(got, method) : NULL;

		if ((v == NULL) || !CFEqual(v, dhcp)) {
			fail("protocol configuration did not round-trip");
			goto out;
		}
	}
	if (!SCNetworkProtocolGetEnabled(proto)) {
		fail("a new protocol is not enabled");
		goto out;
	}
	if (!SCNetworkProtocolSetEnabled(proto, FALSE) ||
	    SCNetworkProtocolGetEnabled(proto)) {
		fail("SCNetworkProtocolSetEnabled(FALSE) did not take");
		goto out;
	}
	if (!SCNetworkProtocolSetEnabled(proto, TRUE) ||
	    !SCNetworkProtocolGetEnabled(proto)) {
		fail("SCNetworkProtocolSetEnabled(TRUE) did not take");
		goto out;
	}

	protos = SCNetworkServiceCopyProtocols(svc);
	if ((protos == NULL) || (CFArrayGetCount(protos) != 1)) {
		fail("SCNetworkServiceCopyProtocols count wrong");
		goto out;
	}
	printf("  AddProtocolType/Protocol*: IPv4 protocol configured\n");

	/* commit, reopen, confirm the service persisted */
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

	svc2 = SCNetworkServiceCopy(prefs, savedID);
	if (svc2 == NULL) {
		fail("SCNetworkServiceCopy after reopen failed");
		goto out;
	}
	if (!CFEqual(SCNetworkServiceGetName(svc2), wantName)) {
		fail("service name did not persist");
		goto out;
	}
	iface = SCNetworkServiceGetInterface(svc2);
	if ((iface == NULL) ||
	    !CFEqual(SCNetworkInterfaceGetBSDName(iface), origBSD)) {
		fail("service interface did not persist");
		goto out;
	}
	proto2 = SCNetworkServiceCopyProtocol(svc2, kSCNetworkProtocolTypeIPv4);
	if (proto2 == NULL) {
		fail("protocol did not persist");
		goto out;
	}
	{
		CFDictionaryRef	got = SCNetworkProtocolGetConfiguration(proto2);
		CFTypeRef	v   = (got != NULL)
				      ? CFDictionaryGetValue(got, method) : NULL;

		if ((v == NULL) || !CFEqual(v, dhcp)) {
			fail("protocol configuration did not persist");
			goto out;
		}
	}
	all2 = SCNetworkServiceCopyAll(prefs);
	if ((all2 == NULL) || (CFArrayGetCount(all2) < 1)) {
		fail("SCNetworkServiceCopyAll did not list the service");
		goto out;
	}
	printf("  CommitChanges/reopen: service persisted\n");

	/* remove the protocol, then the service */
	if (!SCNetworkServiceRemoveProtocolType(svc2,
						kSCNetworkProtocolTypeIPv4)) {
		fail("SCNetworkServiceRemoveProtocolType failed");
		goto out;
	}
	if (SCNetworkServiceCopyProtocol(svc2,
					 kSCNetworkProtocolTypeIPv4) != NULL) {
		fail("protocol still present after removal");
		goto out;
	}
	if (!SCNetworkServiceRemove(svc2)) {
		fail("SCNetworkServiceRemove failed");
		goto out;
	}
	if (SCNetworkServiceCopy(prefs, savedID) != NULL) {
		fail("service still present after removal");
		goto out;
	}
	printf("  RemoveProtocolType/ServiceRemove: torn down\n");

	printf("SC-NETSVC-OK: SCNetworkService + SCNetworkProtocol work\n");
	rc = 0;

    out :
	fflush(stdout);
	if (config != NULL)	CFRelease(config);
	if (proto != NULL)	CFRelease(proto);
	if (proto2 != NULL)	CFRelease(proto2);
	if (protos != NULL)	CFRelease(protos);
	if (svc != NULL)	CFRelease(svc);
	if (svc2 != NULL)	CFRelease(svc2);
	if (all != NULL)	CFRelease(all);
	if (all2 != NULL)	CFRelease(all2);
	if (prefs != NULL)	CFRelease(prefs);
	if (savedID != NULL)	CFRelease(savedID);
	if (origBSD != NULL)	CFRelease(origBSD);
	CFRelease(name);
	CFRelease(prefsID);
	CFRelease(wantName);
	CFRelease(method);
	CFRelease(dhcp);
	return rc;
}
