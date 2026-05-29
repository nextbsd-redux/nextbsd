/*
 * freebsd-launchd-mach hostnamed freebsd-shim.
 *
 * Darwin's <netdb_async.h> exposes the asynchronous resolver API
 * (getaddrinfo_async_start / dns_async_start). Apple's set-hostname.c
 * #includes the header but does not call any of its functions — the
 * reverse-DNS path goes through SCNetworkReachability + the libSC
 * libdns_sd PTR shim. Provide an empty stub so the include resolves.
 */
#ifndef _FREEBSD_LAUNCHD_MACH_NETDB_ASYNC_H_
#define _FREEBSD_LAUNCHD_MACH_NETDB_ASYNC_H_
/* deliberately empty */
#endif
