/*
 * SCNetworkInterface.c — the SCNetworkInterface object (freebsd-launchd-
 * mach port of Apple's SystemConfiguration.fproj SCNetworkInterface.c).
 *
 * An SCNetworkInterface is a CoreFoundation-typed handle to a network
 * interface — its type (Ethernet, Wi-Fi, ...), BSD name, hardware (MAC)
 * address, and the protocols that can be configured on it.
 *
 * Apple's SCNetworkInterface.c enumerates the hardware by walking the
 * IORegistry (IONetworkInterface nodes, USB / PCI / Thunderbolt device
 * matching) — there is no IOKit on FreeBSD, so SCNetworkInterfaceCopyAll
 * here is a native reimplementation over getifaddrs(3): each AF_LINK
 * record is one interface, and its sockaddr_dl carries the link-layer
 * type and address. Layered (virtual) interfaces — Bond / Bridge / VLAN
 * and the PPP family — are a later iteration; iter 2 covers the leaf
 * hardware interfaces and their accessors.
 */

#include "SCNetworkConfigurationInternal.h"
#include "SCInternal.h"

#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>


#pragma mark -
#pragma mark CoreFoundation runtime type

static CFTypeID	__kSCNetworkInterfaceTypeID	= _kCFRuntimeNotATypeID;

/* NULL-safe CFEqual: both NULL is equal, one NULL is not. */
static Boolean
__cfEqual(CFTypeRef a, CFTypeRef b)
{
	if (a == b) {
		return TRUE;
	}
	if ((a == NULL) || (b == NULL)) {
		return FALSE;
	}
	return CFEqual(a, b);
}

static void
__SCNetworkInterfaceDeallocate(CFTypeRef cf)
{
	SCNetworkInterfacePrivateRef	ip	= (SCNetworkInterfacePrivateRef)cf;

	if (ip->interface_type != NULL)		CFRelease(ip->interface_type);
	if (ip->entity_device != NULL)		CFRelease(ip->entity_device);
	if (ip->name != NULL)			CFRelease(ip->name);
	if (ip->localized_name != NULL)		CFRelease(ip->localized_name);
	if (ip->address != NULL)		CFRelease(ip->address);
	if (ip->addressString != NULL)		CFRelease(ip->addressString);
	if (ip->interface != NULL)		CFRelease(ip->interface);
	if (ip->prefs != NULL)			CFRelease(ip->prefs);
	if (ip->serviceID != NULL)		CFRelease(ip->serviceID);
	if (ip->entity_type != NULL)		CFRelease(ip->entity_type);
	if (ip->entity_subtype != NULL)		CFRelease(ip->entity_subtype);
	if (ip->supported_interface_types != NULL)
		CFRelease(ip->supported_interface_types);
	if (ip->supported_protocol_types != NULL)
		CFRelease(ip->supported_protocol_types);
}

/*
 * Two interfaces are equal iff they are the same type, the same BSD
 * device, and the same layering — so an SCNetworkInterface read back
 * from a service compares equal to the enumerated hardware interface.
 */
static Boolean
__SCNetworkInterfaceEqual(CFTypeRef cf1, CFTypeRef cf2)
{
	SCNetworkInterfacePrivateRef	if1	= (SCNetworkInterfacePrivateRef)cf1;
	SCNetworkInterfacePrivateRef	if2	= (SCNetworkInterfacePrivateRef)cf2;

	if (if1 == if2) {
		return TRUE;
	}
	if (!__cfEqual(if1->interface_type, if2->interface_type)) {
		return FALSE;
	}
	if (!__cfEqual(if1->entity_device, if2->entity_device)) {
		return FALSE;
	}
	if (!__cfEqual(if1->interface, if2->interface)) {
		return FALSE;
	}
	return TRUE;
}

static CFHashCode
__SCNetworkInterfaceHash(CFTypeRef cf)
{
	SCNetworkInterfacePrivateRef	ip	= (SCNetworkInterfacePrivateRef)cf;

	return (ip->entity_device != NULL) ? CFHash(ip->entity_device) : 0;
}

static CFStringRef
__SCNetworkInterfaceCopyDescription(CFTypeRef cf)
{
	SCNetworkInterfacePrivateRef	ip	= (SCNetworkInterfacePrivateRef)cf;

	return CFStringCreateWithFormat(NULL, NULL,
			CFSTR("<SCNetworkInterface %@ (%@)>"),
			(ip->entity_device != NULL)
				? ip->entity_device : CFSTR("?"),
			(ip->interface_type != NULL)
				? ip->interface_type : CFSTR("?"));
}

static const CFRuntimeClass __SCNetworkInterfaceClass = {
	.version	= 0,
	.className	= "SCNetworkInterface",
	.finalize	= __SCNetworkInterfaceDeallocate,
	.equal		= __SCNetworkInterfaceEqual,
	.hash		= __SCNetworkInterfaceHash,
	.copyDebugDesc	= __SCNetworkInterfaceCopyDescription,
};

static void
__SCNetworkInterfaceClassInitialize(void)
{
	__kSCNetworkInterfaceTypeID =
		_CFRuntimeRegisterClass(&__SCNetworkInterfaceClass);
}

CFTypeID
SCNetworkInterfaceGetTypeID(void)
{
	static pthread_once_t	once	= PTHREAD_ONCE_INIT;

	(void) pthread_once(&once, __SCNetworkInterfaceClassInitialize);
	return __kSCNetworkInterfaceTypeID;
}

/* isA_SCNetworkInterface() is a shared inline in the internal header. */

SCNetworkInterfacePrivateRef
__SCNetworkInterfaceCreatePrivate(CFAllocatorRef allocator)
{
	SCNetworkInterfacePrivateRef	ip;
	CFIndex				extra;

	extra = sizeof(SCNetworkInterfacePrivate) - sizeof(CFRuntimeBase);
	ip = (SCNetworkInterfacePrivateRef)
	     _CFRuntimeCreateInstance(allocator,
				      SCNetworkInterfaceGetTypeID(),
				      extra, NULL);
	if (ip == NULL) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}

	ip->interface_type		= NULL;
	ip->entity_device		= NULL;
	ip->name			= NULL;
	ip->localized_name		= NULL;
	ip->address			= NULL;
	ip->addressString		= NULL;
	ip->builtin			= FALSE;
	ip->interface			= NULL;
	ip->prefs			= NULL;
	ip->serviceID			= NULL;
	ip->entity_type			= NULL;
	ip->entity_subtype		= NULL;
	ip->supported_interface_types	= NULL;
	ip->supported_protocol_types	= NULL;
	return ip;
}


#pragma mark -
#pragma mark Hardware enumeration (getifaddrs)

/*
 * BSD-name prefixes of the pseudo / virtual interfaces the kernel
 * exposes alongside real hardware. SCNetworkInterfaceCopyAll skips
 * them; the layered (Bond / Bridge / VLAN) interfaces are modelled
 * separately in a later iteration. "wlan" is deliberately absent — a
 * FreeBSD Wi-Fi interface is a wlanN clone and is a real interface.
 */
static const char * const __virtualInterfacePrefixes[] = {
	"lo", "bridge", "vlan", "tap", "tun", "gif", "gre", "epair",
	"pflog", "pfsync", "ipfw", "stf", "faith", "enc", "ng", "ovpn",
	"wg", "ipsec", "disc", "vmnet",
};

static Boolean
__SCNetworkInterfaceIsVirtual(const char *name)
{
	size_t	i;

	for (i = 0; i < (sizeof(__virtualInterfacePrefixes) /
			 sizeof(__virtualInterfacePrefixes[0])); i++) {
		const char	*prefix	= __virtualInterfacePrefixes[i];

		if (strncmp(name, prefix, strlen(prefix)) == 0) {
			return TRUE;
		}
	}
	return FALSE;
}

/*
 * Map a BSD interface to an SCNetworkInterface type, or NULL if it is
 * not a kind iter 2 models. FreeBSD presents 802.11 with Ethernet
 * framing, so a wlanN clone reports IFT_ETHER — the name prefix is the
 * reliable Wi-Fi tell.
 */
static CFStringRef
__SCNetworkInterfaceTypeForLink(const char *name, int sdl_type)
{
	if (strncmp(name, "wlan", 4) == 0) {
		return kSCNetworkInterfaceTypeIEEE80211;
	}
	switch (sdl_type) {
	case IFT_ETHER :
		return kSCNetworkInterfaceTypeEthernet;
	case IFT_IEEE80211 :
		return kSCNetworkInterfaceTypeIEEE80211;
	case IFT_IEEE1394 :
		return kSCNetworkInterfaceTypeFireWire;
	default :
		return NULL;
	}
}

/* "xx:xx:xx:xx:xx:xx" for a link-layer address (lowercase hex). */
static CFStringRef
__SCNetworkInterfaceAddressString(const uint8_t *bytes, int len)
{
	CFMutableStringRef	s;
	int			i;

	if ((bytes == NULL) || (len <= 0)) {
		return NULL;
	}
	s = CFStringCreateMutable(NULL, 0);
	for (i = 0; i < len; i++) {
		CFStringAppendFormat(s, NULL, CFSTR("%s%02x"),
				     (i == 0) ? "" : ":", bytes[i]);
	}
	return s;
}

/* a human-readable name for an interface type */
static CFStringRef
__SCNetworkInterfaceLocalizedName(CFStringRef type)
{
	const char	*s	= "Network";

	if (CFEqual(type, kSCNetworkInterfaceTypeEthernet)) {
		s = "Ethernet";
	} else if (CFEqual(type, kSCNetworkInterfaceTypeIEEE80211)) {
		s = "Wi-Fi";
	} else if (CFEqual(type, kSCNetworkInterfaceTypeFireWire)) {
		s = "FireWire";
	} else if (CFEqual(type, kSCNetworkInterfaceTypeLoopback)) {
		s = "Loopback";
	}
	return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
}

/* the protocol types configurable on a hardware network interface */
static CFArrayRef
__SCNetworkInterfaceCopyProtocolTypes(void)
{
	const void	*protos[5];

	protos[0] = kSCNetworkProtocolTypeIPv4;
	protos[1] = kSCNetworkProtocolTypeIPv6;
	protos[2] = kSCNetworkProtocolTypeDNS;
	protos[3] = kSCNetworkProtocolTypeProxies;
	protos[4] = kSCNetworkProtocolTypeSMB;
	return CFArrayCreate(NULL, protos, 5, &kCFTypeArrayCallBacks);
}

/* build an interface object from one getifaddrs AF_LINK record */
static SCNetworkInterfaceRef
__SCNetworkInterfaceCreateForLink(const char *name,
				  const struct sockaddr_dl *sdl)
{
	CFStringRef			type;
	SCNetworkInterfacePrivateRef	ip;

	type = __SCNetworkInterfaceTypeForLink(name, sdl->sdl_type);
	if (type == NULL) {
		/* not a hardware interface kind iter 2 models */
		return NULL;
	}

	ip = __SCNetworkInterfaceCreatePrivate(NULL);
	if (ip == NULL) {
		return NULL;
	}

	ip->interface_type = (CFStringRef)CFRetain(type);
	ip->entity_device  = CFStringCreateWithCString(NULL, name,
						       kCFStringEncodingUTF8);
	ip->builtin        = TRUE;

	/* link-layer (hardware) address */
	if (sdl->sdl_alen > 0) {
		const uint8_t	*lladdr	= (const uint8_t *)LLADDR(sdl);

		ip->address       = CFDataCreate(NULL, lladdr, sdl->sdl_alen);
		ip->addressString = __SCNetworkInterfaceAddressString(lladdr,
							       sdl->sdl_alen);
	}

	/* display names */
	ip->localized_name = __SCNetworkInterfaceLocalizedName(type);
	ip->name           = (CFStringRef)CFRetain(ip->localized_name);

	/* the protocols configurable on a hardware interface */
	ip->supported_protocol_types = __SCNetworkInterfaceCopyProtocolTypes();

	return (SCNetworkInterfaceRef)ip;
}

CFArrayRef
SCNetworkInterfaceCopyAll(void)
{
	CFMutableArrayRef	all;
	struct ifaddrs		*ifap	= NULL;
	struct ifaddrs		*ifa;

	all = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	if (all == NULL) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}
	if (getifaddrs(&ifap) != 0) {
		/* nothing visible — an empty list is still a valid answer */
		return all;
	}

	for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
		struct sockaddr_dl	*sdl;
		SCNetworkInterfaceRef	interface;

		/* one AF_LINK record carries the link-layer info per device */
		if ((ifa->ifa_addr == NULL) ||
		    (ifa->ifa_addr->sa_family != AF_LINK)) {
			continue;
		}
		if ((ifa->ifa_flags & IFF_LOOPBACK) != 0) {
			continue;
		}
		if (__SCNetworkInterfaceIsVirtual(ifa->ifa_name)) {
			continue;
		}

		sdl = (struct sockaddr_dl *)(void *)ifa->ifa_addr;
		interface = __SCNetworkInterfaceCreateForLink(ifa->ifa_name,
							      sdl);
		if (interface != NULL) {
			CFArrayAppendValue(all, interface);
			CFRelease(interface);
		}
	}

	freeifaddrs(ifap);
	return all;
}


#pragma mark -
#pragma mark Accessors

CFStringRef
SCNetworkInterfaceGetBSDName(SCNetworkInterfaceRef interface)
{
	if (!isA_SCNetworkInterface(interface)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	return ((SCNetworkInterfacePrivateRef)interface)->entity_device;
}

CFStringRef
SCNetworkInterfaceGetInterfaceType(SCNetworkInterfaceRef interface)
{
	if (!isA_SCNetworkInterface(interface)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	return ((SCNetworkInterfacePrivateRef)interface)->interface_type;
}

CFStringRef
SCNetworkInterfaceGetHardwareAddressString(SCNetworkInterfaceRef interface)
{
	if (!isA_SCNetworkInterface(interface)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	return ((SCNetworkInterfacePrivateRef)interface)->addressString;
}

CFStringRef
SCNetworkInterfaceGetLocalizedDisplayName(SCNetworkInterfaceRef interface)
{
	if (!isA_SCNetworkInterface(interface)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	return ((SCNetworkInterfacePrivateRef)interface)->localized_name;
}

SCNetworkInterfaceRef
SCNetworkInterfaceGetInterface(SCNetworkInterfaceRef interface)
{
	if (!isA_SCNetworkInterface(interface)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	return ((SCNetworkInterfacePrivateRef)interface)->interface;
}

CFArrayRef
SCNetworkInterfaceGetSupportedInterfaceTypes(SCNetworkInterfaceRef interface)
{
	if (!isA_SCNetworkInterface(interface)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	return ((SCNetworkInterfacePrivateRef)interface)->supported_interface_types;
}

CFArrayRef
SCNetworkInterfaceGetSupportedProtocolTypes(SCNetworkInterfaceRef interface)
{
	if (!isA_SCNetworkInterface(interface)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	return ((SCNetworkInterfacePrivateRef)interface)->supported_protocol_types;
}


#pragma mark -
#pragma mark Preferences entity bridge

/*
 * Serialize an interface into the dictionary stored at a service's
 * "Interface" path. A hardware interface needs only its type and BSD
 * device name; the localized name is kept so a removed interface still
 * has a readable label. (Apple's entity also carries IORegistry detail
 * — irrelevant without IOKit.)
 */
CFDictionaryRef
__SCNetworkInterfaceCopyInterfaceEntity(SCNetworkInterfaceRef interface)
{
	SCNetworkInterfacePrivateRef	ip	= (SCNetworkInterfacePrivateRef)interface;
	CFMutableDictionaryRef		entity;

	entity = CFDictionaryCreateMutable(NULL, 0,
					   &kCFTypeDictionaryKeyCallBacks,
					   &kCFTypeDictionaryValueCallBacks);
	if (ip->interface_type != NULL) {
		CFDictionarySetValue(entity, kSCPropNetInterfaceType,
				     ip->interface_type);
		CFDictionarySetValue(entity, kSCPropNetInterfaceHardware,
				     ip->interface_type);
	}
	if (ip->entity_device != NULL) {
		CFDictionarySetValue(entity, kSCPropNetInterfaceDeviceName,
				     ip->entity_device);
	}
	if (ip->localized_name != NULL) {
		CFDictionarySetValue(entity, kSCPropUserDefinedName,
				     ip->localized_name);
	}
	return entity;
}

/*
 * Rebuild an interface from the dictionary stored at a service's
 * "Interface" path. The reconstructed interface has no hardware address
 * (the entity does not store one — it is matched live against the
 * enumerated hardware). `service`, when non-NULL, binds the interface
 * to its preferences session.
 */
SCNetworkInterfaceRef
_SCNetworkInterfaceCreateWithEntity(CFAllocatorRef allocator,
				    CFDictionaryRef entity,
				    SCNetworkServiceRef service)
{
	SCNetworkInterfacePrivateRef	ip;
	CFStringRef			type;
	CFStringRef			device;

	if ((entity == NULL) ||
	    (CFGetTypeID(entity) != CFDictionaryGetTypeID())) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	type   = CFDictionaryGetValue(entity, kSCPropNetInterfaceType);
	device = CFDictionaryGetValue(entity, kSCPropNetInterfaceDeviceName);

	ip = __SCNetworkInterfaceCreatePrivate(allocator);
	if (ip == NULL) {
		return NULL;
	}

	if ((type != NULL) && (CFGetTypeID(type) == CFStringGetTypeID())) {
		ip->interface_type = (CFStringRef)CFRetain(type);
	} else {
		ip->interface_type =
			(CFStringRef)CFRetain(kSCNetworkInterfaceTypeEthernet);
	}
	if ((device != NULL) && (CFGetTypeID(device) == CFStringGetTypeID())) {
		ip->entity_device = (CFStringRef)CFRetain(device);
	}
	ip->builtin		= TRUE;
	ip->localized_name	= __SCNetworkInterfaceLocalizedName(ip->interface_type);
	ip->name		= (CFStringRef)CFRetain(ip->localized_name);
	ip->supported_protocol_types = __SCNetworkInterfaceCopyProtocolTypes();

	if (service != NULL) {
		SCNetworkServicePrivateRef	sp =
			(SCNetworkServicePrivateRef)service;

		if (sp->prefs != NULL) {
			ip->prefs = (SCPreferencesRef)CFRetain(sp->prefs);
		}
		if (sp->serviceID != NULL) {
			ip->serviceID = (CFStringRef)CFRetain(sp->serviceID);
		}
	}
	return (SCNetworkInterfaceRef)ip;
}
