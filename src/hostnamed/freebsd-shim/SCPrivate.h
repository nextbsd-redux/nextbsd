/*
 * freebsd-launchd-mach hostnamed freebsd-shim.
 *
 * SCPrivate.h SPI subset that Apple's set-hostname.c calls. Apple's
 * real header lives in SystemConfiguration.framework's private
 * include tree; we ship the small surface set-hostname.c uses
 * (_SC_string_to_sockaddr, _SC_cfstring_to_cstring,
 * _SC_CFStringIsValidDNSName) plus the notification-token constant
 * (_SC_NOTIFY_NETWORK_CHANGE).
 */

#ifndef _FREEBSD_LAUNCHD_MACH_SC_PRIVATE_H_
#define _FREEBSD_LAUNCHD_MACH_SC_PRIVATE_H_

#include <CoreFoundation/CoreFoundation.h>
#include <sys/socket.h>

/*
 * _SC_string_to_sockaddr — parse a numeric address string (IPv4 or
 * IPv6 per `family`; AF_UNSPEC tries both) into a struct sockaddr.
 * Returns the struct's sa_family on success (caller passes a buffer
 * sized for sockaddr_in / sockaddr_in6); returns NULL on parse error.
 * The Apple SPI returns a pointer-cast of the supplied buffer; our
 * shim follows the same convention.
 */
void *	_SC_string_to_sockaddr(const char *str, sa_family_t family,
	    void *buf, size_t bufsize);

/*
 * _SC_cfstring_to_cstring — copy a CFStringRef into a C buffer using
 * the requested encoding. Returns `buf` on success, NULL on encoding
 * failure or undersized buffer. If `buf` is NULL, allocates a
 * malloc()'d buffer sized to the encoded length plus terminator.
 */
char *	_SC_cfstring_to_cstring(CFStringRef cfstr, char *buf, CFIndex bufsize,
	    CFStringEncoding encoding);

/*
 * _SC_CFStringIsValidDNSName — TRUE iff the CFString is a syntactically
 * valid RFC 1035 LDH name (letters/digits/hyphens, no leading or
 * trailing hyphen, total length 1..253, label length 1..63).
 */
Boolean	_SC_CFStringIsValidDNSName(CFStringRef cfstr);

/*
 * _SC_NOTIFY_NETWORK_CHANGE — the Apple-canonical notify(3) token
 * fired whenever network state changes (configd's PF_ROUTE monitor
 * + interface flags + lease state). set-hostname.c re-runs its
 * decision engine on this token in addition to its SCDS subscriptions.
 */
#define	_SC_NOTIFY_NETWORK_CHANGE	"com.apple.system.config.network_change"

#endif	/* _FREEBSD_LAUNCHD_MACH_SC_PRIVATE_H_ */
