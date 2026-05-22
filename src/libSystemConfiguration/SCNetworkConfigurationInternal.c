/*
 * SCNetworkConfigurationInternal.c — definitions shared across the
 * SCNetworkConfiguration sources (freebsd-launchd-mach port).
 *
 * This file defines the kSCNetworkInterfaceType* and kSCNetworkProtocol
 * Type* constant CFStrings. Apple spreads these across SCNetworkInterface.c
 * and SCNetworkProtocol.c; collecting them here keeps every constant in
 * one translation unit as the object families land iteration by
 * iteration. The string values are the ones written into the preferences
 * plist, so they match Apple's exactly.
 */

#include "SCNetworkConfigurationInternal.h"

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SCNetworkConfiguration.h>

const CFStringRef kSCNetworkInterfaceType6to4		= CFSTR("6to4");
const CFStringRef kSCNetworkInterfaceTypeBluetooth	= CFSTR("Bluetooth");
const CFStringRef kSCNetworkInterfaceTypeBond		= CFSTR("Bond");
const CFStringRef kSCNetworkInterfaceTypeBridge		= CFSTR("Bridge");
const CFStringRef kSCNetworkInterfaceTypeEthernet	= CFSTR("Ethernet");
const CFStringRef kSCNetworkInterfaceTypeFireWire	= CFSTR("FireWire");
const CFStringRef kSCNetworkInterfaceTypeIEEE80211	= CFSTR("IEEE80211");
const CFStringRef kSCNetworkInterfaceTypeIPSec		= CFSTR("IPSec");
const CFStringRef kSCNetworkInterfaceTypeL2TP		= CFSTR("L2TP");
const CFStringRef kSCNetworkInterfaceTypeLoopback	= CFSTR("Loopback");
const CFStringRef kSCNetworkInterfaceTypeModem		= CFSTR("Modem");
const CFStringRef kSCNetworkInterfaceTypePPP		= CFSTR("PPP");
const CFStringRef kSCNetworkInterfaceTypeSerial		= CFSTR("Serial");
const CFStringRef kSCNetworkInterfaceTypeVLAN		= CFSTR("VLAN");
const CFStringRef kSCNetworkInterfaceTypeWWAN		= CFSTR("WWAN");
const CFStringRef kSCNetworkInterfaceTypeIPv4		= CFSTR("IPv4");

const CFStringRef kSCNetworkProtocolTypeDNS		= CFSTR("DNS");
const CFStringRef kSCNetworkProtocolTypeIPv4		= CFSTR("IPv4");
const CFStringRef kSCNetworkProtocolTypeIPv6		= CFSTR("IPv6");
const CFStringRef kSCNetworkProtocolTypeProxies		= CFSTR("Proxies");
const CFStringRef kSCNetworkProtocolTypeSMB		= CFSTR("SMB");


CFStringRef
__SCNetworkServiceEntityPath(CFStringRef serviceID, CFStringRef entity)
{
	if (entity == NULL) {
		return CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@/%@"),
						kSCPrefNetworkServices,
						serviceID);
	}
	return CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@/%@/%@"),
					kSCPrefNetworkServices,
					serviceID, entity);
}
