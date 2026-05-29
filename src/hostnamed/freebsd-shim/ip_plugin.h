/*
 * freebsd-launchd-mach hostnamed freebsd-shim.
 *
 * On real macOS, set-hostname.c lives inside the IPMonitor configd
 * plugin and `#include "ip_plugin.h"` pulls in:
 *   - my_log() — routed to ASL via _SC_log.
 *   - extern copy_dhcp_hostname(serviceID) — reads DHCP lease state
 *     out of IPMonitor's per-service internals and returns Option 12.
 *   - extern check_if_service_expensive(serviceID) — TRUE on
 *     metered/cellular/hotspot services.
 *
 * Our build is standalone; we provide each of those plus the
 * synthesis fallback set-hostname.c's "localhost" carry calls into.
 */

#ifndef _FREEBSD_LAUNCHD_MACH_IP_PLUGIN_H_
#define _FREEBSD_LAUNCHD_MACH_IP_PLUGIN_H_

#include <CoreFoundation/CoreFoundation.h>
#include <syslog.h>

/*
 * my_log — route set-hostname.c's diagnostic output through hostnamed's
 * xlog(). hostnamed_xlog_cf does CFStringRef-aware formatting; on
 * non-CFString arguments it falls back to %s / %d / %u.
 */
extern void	hostnamed_xlog_cf(int level, CFStringRef format, ...);
#undef	my_log
#define	my_log(__level, __fmt, ...)	\
	hostnamed_xlog_cf((__level), CFSTR(__fmt), ## __VA_ARGS__)

/*
 * IPMonitor-internal helpers set-hostname.c declares extern.
 *   copy_dhcp_hostname(serviceID) — reads DHCP Option 12 (host-name)
 *     from State:/Network/Service/<serviceID>/DHCP. CFStringRef caller
 *     releases, or NULL.
 *   check_if_service_expensive(serviceID) — TRUE if metered. Always
 *     FALSE on FreeBSD (no metered-network concept).
 */
CFStringRef	copy_dhcp_hostname(CFStringRef serviceID);
Boolean		check_if_service_expensive(CFStringRef serviceID);

/*
 * freebsd_synthesize_hostname — slug+suffix synthesis from SMBIOS +
 * NIC MAC + kern.hostuuid. Used by set-hostname.c's "localhost"
 * fallback substitute per hostnamed plan §Q6. Caller releases.
 */
CFStringRef	freebsd_synthesize_hostname(void);

#endif	/* _FREEBSD_LAUNCHD_MACH_IP_PLUGIN_H_ */
