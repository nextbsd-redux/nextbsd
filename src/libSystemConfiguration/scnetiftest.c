/*
 * scnetiftest.c — libSystemConfiguration SCNetworkConfiguration iter 2
 * test client: SCNetworkInterface hardware enumeration.
 *
 * Exercises SCNetworkInterfaceCopyAll and the interface accessors
 * against the live system. The QEMU guest is booted with one e1000
 * NIC, so the enumeration must report at least one Ethernet interface
 * with a hardware address; loopback must be excluded. Every returned
 * interface must be well-formed (a BSD name, a recognized type, the
 * IPv4 protocol in its supported set).
 *
 * run.sh runs it and boot-test.sh gates on the SC-NETIF-OK / -FAIL
 * marker.
 */

#include <SystemConfiguration/SystemConfiguration.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>

static void
fail(const char *msg)
{
	printf("SC-NETIF-FAIL: %s\n", msg);
	fflush(stdout);
}

/* TRUE iff `array` (of CFString) contains `want` */
static Boolean
array_has(CFArrayRef array, CFStringRef want)
{
	CFIndex	i, n;

	if (array == NULL) {
		return FALSE;
	}
	n = CFArrayGetCount(array);
	for (i = 0; i < n; i++) {
		if (CFEqual(CFArrayGetValueAtIndex(array, i), want)) {
			return TRUE;
		}
	}
	return FALSE;
}

/* TRUE iff `s` is one of the recognized interface types */
static Boolean
is_known_type(CFStringRef s)
{
	return (CFEqual(s, kSCNetworkInterfaceTypeEthernet) ||
		CFEqual(s, kSCNetworkInterfaceTypeIEEE80211) ||
		CFEqual(s, kSCNetworkInterfaceTypeFireWire));
}

int
main(void)
{
	CFArrayRef	all	= NULL;
	CFStringRef	lo0	= NULL;
	CFIndex		i, n;
	int		ethernet = 0;
	int		rc	= 1;

	printf("scnetiftest: SCNetworkInterface enumeration\n");
	fflush(stdout);

	lo0 = CFStringCreateWithCString(NULL, "lo0", kCFStringEncodingUTF8);

	/* the type identifier must be stable */
	if (SCNetworkInterfaceGetTypeID() != SCNetworkInterfaceGetTypeID()) {
		fail("SCNetworkInterfaceGetTypeID not stable");
		goto out;
	}

	all = SCNetworkInterfaceCopyAll();
	if (all == NULL) {
		fail("SCNetworkInterfaceCopyAll returned NULL");
		goto out;
	}
	n = CFArrayGetCount(all);
	printf("  SCNetworkInterfaceCopyAll: %ld interface(s)\n", (long)n);
	if (n < 1) {
		fail("no interfaces enumerated (expected the e1000 NIC)");
		goto out;
	}

	for (i = 0; i < n; i++) {
		SCNetworkInterfaceRef	iface;
		CFStringRef		bsdName, type, mac, disp;

		iface = (SCNetworkInterfaceRef)CFArrayGetValueAtIndex(all, i);
		if (CFGetTypeID(iface) != SCNetworkInterfaceGetTypeID()) {
			fail("array element is not an SCNetworkInterface");
			goto out;
		}

		bsdName = SCNetworkInterfaceGetBSDName(iface);
		type    = SCNetworkInterfaceGetInterfaceType(iface);
		if ((bsdName == NULL) || (type == NULL)) {
			fail("interface missing a BSD name or type");
			goto out;
		}
		if (CFEqual(bsdName, lo0)) {
			fail("loopback was not excluded from CopyAll");
			goto out;
		}
		if (!is_known_type(type)) {
			fail("interface has an unrecognized type");
			goto out;
		}
		if (!array_has(SCNetworkInterfaceGetSupportedProtocolTypes(iface),
			       kSCNetworkProtocolTypeIPv4)) {
			fail("IPv4 missing from supported protocol types");
			goto out;
		}
		if (SCNetworkInterfaceGetInterface(iface) != NULL) {
			fail("a hardware interface reported a sub-interface");
			goto out;
		}

		mac  = SCNetworkInterfaceGetHardwareAddressString(iface);
		disp = SCNetworkInterfaceGetLocalizedDisplayName(iface);
		{
			char	nbuf[64] = "?";

			CFStringGetCString(bsdName, nbuf, sizeof(nbuf),
					   kCFStringEncodingUTF8);
			printf("  %-8s type=%-10s mac=%s\n", nbuf,
			       CFEqual(type, kSCNetworkInterfaceTypeEthernet)
				       ? "Ethernet"
			       : CFEqual(type, kSCNetworkInterfaceTypeIEEE80211)
				       ? "IEEE80211" : "FireWire",
			       (mac != NULL) ? "present" : "(none)");
		}

		if (CFEqual(type, kSCNetworkInterfaceTypeEthernet)) {
			/* the e1000 NIC: must have a usable MAC + name */
			if ((mac == NULL) ||
			    (CFStringGetLength(mac) == 0) ||
			    (CFStringFind(mac, CFSTR(":"), 0).location
			     == kCFNotFound)) {
				fail("Ethernet interface lacks a MAC string");
				goto out;
			}
			if ((disp == NULL) ||
			    (CFStringGetLength(disp) == 0)) {
				fail("Ethernet interface lacks a display name");
				goto out;
			}
			ethernet++;
		}
	}

	if (ethernet < 1) {
		fail("no Ethernet interface found (expected the e1000 NIC)");
		goto out;
	}

	printf("SC-NETIF-OK: SCNetworkInterface enumeration works\n");
	rc = 0;

    out :
	fflush(stdout);
	if (all != NULL)	CFRelease(all);
	if (lo0 != NULL)	CFRelease(lo0);
	return rc;
}
