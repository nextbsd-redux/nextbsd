/*
 * freebsd-launchd-mach hostnamed freebsd-shim.
 *
 * Empty stub for Apple's <SystemConfiguration/SCDynamicStoreCopyDHCPInfo.h>.
 * Apple's real header declares DHCP-info accessors backed by IPMonitor;
 * set-hostname.c #includes it but does not call anything from it — the
 * file's DHCP reads go through the extern copy_dhcp_hostname() from
 * ip_plugin.h instead. Provide an empty header so the include resolves.
 */
#ifndef _FREEBSD_LAUNCHD_MACH_SCDYNAMICSTORE_COPY_DHCP_INFO_H_
#define _FREEBSD_LAUNCHD_MACH_SCDYNAMICSTORE_COPY_DHCP_INFO_H_
/* deliberately empty */
#endif
