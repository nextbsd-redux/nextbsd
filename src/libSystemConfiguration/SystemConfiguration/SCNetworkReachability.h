/*
 * Copyright (c) 2000-2023 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

/*
 * SCNetworkReachability.h — async DNS / reverse-DNS / reachability
 * lookups, dispatched on a libdispatch queue.
 *
 * freebsd-launchd-mach port: the PTR-only subset of Apple's
 * SystemConfiguration.framework SCNetworkReachability.h. The full
 * API supports forward DNS, host-reachability flags, and interface-
 * change notifications; this port covers only what the vendored
 * IPMonitor set-hostname.c uses — async reverse PTR lookup keyed by a
 * sockaddr, with cancellation via CFRelease — backed by libdns_sd's
 * DNSServiceQueryRecord against mDNSResponder. Forward DNS /
 * reachability flags will land when a consumer needs them.
 */

#ifndef _SCNETWORKREACHABILITY_H
#define _SCNETWORKREACHABILITY_H

#include <sys/cdefs.h>
#include <dispatch/dispatch.h>
#include <CoreFoundation/CoreFoundation.h>

/*!
	@typedef SCNetworkReachabilityRef
	@discussion Opaque handle to a reachability target. A CoreFoundation
		type (see SCNetworkReachabilityGetTypeID()) — release with
		CFRelease(). Releasing cancels any in-flight query and
		fires no further callbacks.
 */
typedef const struct __SCNetworkReachability *	SCNetworkReachabilityRef;

/*!
	@typedef SCNetworkReachabilityContext
	@discussion Client-supplied data + lifecycle callbacks. The
		`info` pointer is passed verbatim to the reachability
		callback.
 */
typedef struct {
	CFIndex		version;
	void *		info;
	const void *	(*retain)(const void *info);
	void		(*release)(const void *info);
	CFStringRef	(*copyDescription)(const void *info);
} SCNetworkReachabilityContext;

/*!
	@typedef SCNetworkReachabilityFlags
	@discussion Bitmask describing the resolved reachability state.
		Apple's full enum has many bits; the PTR-only shim only
		ever sets kSCNetworkReachabilityFlagsReachable on success
		and 0 on failure.
 */
typedef uint32_t	SCNetworkReachabilityFlags;

#define	kSCNetworkReachabilityFlagsTransientConnection		(1 << 0)
#define	kSCNetworkReachabilityFlagsReachable			(1 << 1)
#define	kSCNetworkReachabilityFlagsConnectionRequired		(1 << 2)
#define	kSCNetworkReachabilityFlagsConnectionOnTraffic		(1 << 3)
#define	kSCNetworkReachabilityFlagsInterventionRequired		(1 << 4)
#define	kSCNetworkReachabilityFlagsConnectionOnDemand		(1 << 5)
#define	kSCNetworkReachabilityFlagsIsLocalAddress		(1 << 16)
#define	kSCNetworkReachabilityFlagsIsDirect			(1 << 17)

/*!
	@typedef SCNetworkReachabilityCallBack
	@discussion Fired on the dispatch queue (or run loop) the
		target is scheduled on, whenever the resolved state
		changes. For a PTR query, fires once with
		kSCNetworkReachabilityFlagsReachable set when the answer
		arrives, or 0 on timeout / failure.
 */
typedef void (*SCNetworkReachabilityCallBack)(
	SCNetworkReachabilityRef		target,
	SCNetworkReachabilityFlags		flags,
	void *					info);

/*!
	@const kSCNetworkReachabilityOptionPTRAddress
	@discussion Options-dict key for CreateWithOptions. The value is
		a CFData wrapping a struct sockaddr (AF_INET or AF_INET6);
		the shim issues a reverse PTR for that address.
 */
extern const CFStringRef	kSCNetworkReachabilityOptionPTRAddress;

__BEGIN_DECLS

/*!
	@function SCNetworkReachabilityGetTypeID
	@discussion CoreFoundation type ID for SCNetworkReachabilityRef.
 */
CFTypeID
SCNetworkReachabilityGetTypeID	(void);

/*!
	@function SCNetworkReachabilityCreateWithOptions
	@discussion Create a target from an options dictionary. The
		PTR-only port supports a single option key
		kSCNetworkReachabilityOptionPTRAddress. Caller releases.
 */
SCNetworkReachabilityRef
SCNetworkReachabilityCreateWithOptions
				(CFAllocatorRef			allocator,
				 CFDictionaryRef		options);

/*!
	@function SCNetworkReachabilitySetCallback
	@discussion Sets (or clears, with NULL) the callback the target
		fires when its resolved state changes. Must be called
		before SetDispatchQueue. Returns TRUE on success.
 */
Boolean
SCNetworkReachabilitySetCallback
				(SCNetworkReachabilityRef	target,
				 SCNetworkReachabilityCallBack	callout,
				 SCNetworkReachabilityContext *	context);

/*!
	@function SCNetworkReachabilitySetDispatchQueue
	@discussion Schedules callback delivery on a dispatch queue. Pass
		NULL to unschedule (this cancels the in-flight PTR query
		and fires no further callbacks). Returns TRUE on success.
 */
Boolean
SCNetworkReachabilitySetDispatchQueue
				(SCNetworkReachabilityRef	target,
				 dispatch_queue_t		queue);

/*!
	@function SCNetworkReachabilityCopyResolvedAddress
	@discussion After the callback fires with
		kSCNetworkReachabilityFlagsReachable, returns the resolved
		results. For a PTR query, the array contains
		CFStringRef hostnames (length 1 in this port). Returns
		NULL if the query has not completed, timed out, or failed;
		writes the libdns_sd error code into *error_num if non-NULL.
		Caller releases.
 */
CFArrayRef
SCNetworkReachabilityCopyResolvedAddress
				(SCNetworkReachabilityRef	target,
				 int *				error_num);

__END_DECLS

#endif	/* _SCNETWORKREACHABILITY_H */
