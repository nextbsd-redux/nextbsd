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
 * SCNetworkReachability.c — PTR-only port over libdns_sd.
 *
 * The vendored IPMonitor set-hostname.c is the sole consumer at iter
 * 3c: it builds an options dict carrying kSCNetworkReachabilityOption
 * PTRAddress (CFData of struct sockaddr_in), creates a reachability
 * target, sets a callback + dispatch queue, and waits to be told the
 * reverse hostname. On the system this port targets, Apple's
 * SCNetworkReachability goes through libresolv / mDNSResponder via
 * private SPIs we do not have. We back the API instead with libdns_sd's
 * DNSServiceQueryRecord against the already-running mDNSResponder —
 * which gives the same async event-driven contract (callback on a
 * dispatch queue, releasable handle that cancels any in-flight query)
 * without porting the rest of Apple's reachability stack.
 *
 * The full SCNetworkReachability API (forward DNS, host-reachability
 * flags, interface-arrival notifications) is deliberately out of scope
 * — those will land when a consumer needs them.
 */

#include <SystemConfiguration/SCNetworkReachability.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFRuntime.h>

#include <dispatch/dispatch.h>
#include <dns_sd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* CF type registration ------------------------------------------- */

const CFStringRef kSCNetworkReachabilityOptionPTRAddress =
    CFSTR("PTRAddress");

typedef struct __SCNetworkReachability {
	CFRuntimeBase			cfBase;

	/* PTR target sockaddr (from options dict). */
	CFDataRef			ptr_address;

	/* User callback + context. */
	SCNetworkReachabilityCallBack	cb;
	SCNetworkReachabilityContext	cb_ctx;

	/* Dispatch queue + libdns_sd socket source. */
	dispatch_queue_t		queue;
	dispatch_source_t		sd_source;
	DNSServiceRef			sd_ref;

	/* PTR result. CFMutableArray of CFStrings (length 1 on hit). */
	CFMutableArrayRef		resolved;
	SCNetworkReachabilityFlags	flags;
	int				error_num;
} SCNetworkReachabilityPrivate, *SCNetworkReachabilityPrivateRef;

static CFTypeID __kSCNetworkReachabilityTypeID = _kCFRuntimeNotATypeID;

static void	__SCNRStop(SCNetworkReachabilityPrivateRef target);

static void
__SCNRDeallocate(CFTypeRef cf)
{
	SCNetworkReachabilityPrivateRef p = (SCNetworkReachabilityPrivateRef)cf;

	__SCNRStop(p);

	if (p->cb_ctx.release != NULL && p->cb_ctx.info != NULL)
		p->cb_ctx.release(p->cb_ctx.info);
	if (p->ptr_address != NULL)
		CFRelease(p->ptr_address);
	if (p->resolved != NULL)
		CFRelease(p->resolved);
}

static CFStringRef
__SCNRCopyDescription(CFTypeRef cf)
{
	(void)cf;
	return (CFStringCreateWithCString(NULL, "<SCNetworkReachability>",
	    kCFStringEncodingASCII));
}

static const CFRuntimeClass __SCNRClass = {
	.version	= 0,
	.className	= "SCNetworkReachability",
	.finalize	= __SCNRDeallocate,
	.copyDebugDesc	= __SCNRCopyDescription,
};

static void
__SCNRClassInitialize(void)
{
	__kSCNetworkReachabilityTypeID = _CFRuntimeRegisterClass(&__SCNRClass);
}

CFTypeID
SCNetworkReachabilityGetTypeID(void)
{
	static pthread_once_t once = PTHREAD_ONCE_INIT;
	(void)pthread_once(&once, __SCNRClassInitialize);
	return (__kSCNetworkReachabilityTypeID);
}

/* PTR-name helpers ---------------------------------------------- */

/* Build "<d>.<c>.<b>.<a>.in-addr.arpa" from an IPv4 sockaddr. */
static int
build_reverse_inaddr(const struct sockaddr *sa, char *out, size_t outsz)
{
	const struct sockaddr_in *sin;
	const uint8_t *a;
	int n;

	if (sa == NULL || sa->sa_family != AF_INET)
		return (0);	/* AF_INET6 ip6.arpa not needed by iter 3c */
	sin = (const struct sockaddr_in *)(const void *)sa;
	a = (const uint8_t *)&sin->sin_addr;
	n = snprintf(out, outsz, "%u.%u.%u.%u.in-addr.arpa",
	    a[3], a[2], a[1], a[0]);
	return (n > 0 && (size_t)n < outsz);
}

/* Decode the first label of a length-prefixed PTR rdata wire encoding
 * into a CFString. PTR rdata is the wire-format domain name:
 * length-prefixed labels terminated by a zero byte. We stitch the
 * labels back into "<label1>.<label2>..." form. Returns NULL on a
 * malformed encoding or a name pointing past the buffer. */
static CFStringRef
decode_ptr_rdata(const uint8_t *rdata, uint16_t rdlen)
{
	CFMutableStringRef name;
	size_t off = 0;
	int parts = 0;

	if (rdata == NULL || rdlen < 1)
		return (NULL);
	name = CFStringCreateMutable(NULL, 0);
	if (name == NULL)
		return (NULL);
	while (off < rdlen) {
		uint8_t label_len = rdata[off];

		if (label_len == 0)
			break;
		/* Defend against DNS compression pointers (0xC0+). uds_daemon
		 * decompresses, but defend anyway. */
		if (label_len >= 64 ||
		    (uint16_t)(off + 1 + label_len) > rdlen) {
			CFRelease(name);
			return (NULL);
		}
		if (parts > 0)
			CFStringAppendCString(name, ".",
			    kCFStringEncodingASCII);
		{
			char buf[64];
			(void)memcpy(buf, rdata + off + 1, label_len);
			buf[label_len] = '\0';
			CFStringAppendCString(name, buf,
			    kCFStringEncodingUTF8);
		}
		parts++;
		off += 1 + label_len;
	}
	if (parts == 0) {
		CFRelease(name);
		return (NULL);
	}
	return (name);
}

/* libdns_sd PTR callback. Decodes rdata, stashes the hostname, fires
 * the user callback. Runs on the dispatch queue (driven by the
 * DNSServiceProcessResult call inside our dispatch source). */
static void DNSSD_API
ptr_cb(DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t ifIdx,
    DNSServiceErrorType err, const char *fullname, uint16_t rrtype,
    uint16_t rrclass, uint16_t rdlen, const void *rdata, uint32_t ttl,
    void *context)
{
	SCNetworkReachabilityPrivateRef p = context;
	CFStringRef name;

	(void)sdRef;
	(void)flags;
	(void)ifIdx;
	(void)fullname;
	(void)rrtype;
	(void)rrclass;
	(void)ttl;

	if (p == NULL)
		return;
	p->error_num = (int)err;
	if (err != kDNSServiceErr_NoError) {
		p->flags = 0;
	} else {
		name = decode_ptr_rdata(rdata, rdlen);
		if (name != NULL) {
			if (p->resolved == NULL)
				p->resolved = CFArrayCreateMutable(NULL,
				    1, &kCFTypeArrayCallBacks);
			if (p->resolved != NULL)
				CFArrayAppendValue(p->resolved, name);
			CFRelease(name);
			p->flags = kSCNetworkReachabilityFlagsReachable;
		} else {
			p->flags = 0;
		}
	}
	if (p->cb != NULL) {
		p->cb((SCNetworkReachabilityRef)p, p->flags,
		    p->cb_ctx.info);
	}
}

/* Tear down: cancel dispatch source, deallocate libdns_sd, drop queue. */
static void
__SCNRStop(SCNetworkReachabilityPrivateRef p)
{
	if (p->sd_source != NULL) {
		dispatch_source_cancel(p->sd_source);
		dispatch_release(p->sd_source);
		p->sd_source = NULL;
	}
	if (p->sd_ref != NULL) {
		DNSServiceRefDeallocate(p->sd_ref);
		p->sd_ref = NULL;
	}
	if (p->queue != NULL) {
		dispatch_release(p->queue);
		p->queue = NULL;
	}
}

/* Start: build the reverse name, kick the libdns_sd PTR query, wire
 * the libdns_sd socket FD into a dispatch source on the queue. */
static Boolean
__SCNRStart(SCNetworkReachabilityPrivateRef p, dispatch_queue_t queue)
{
	char reverse[128];
	DNSServiceErrorType err;
	int fd;

	if (p->ptr_address == NULL ||
	    CFDataGetLength(p->ptr_address) < (CFIndex)sizeof(struct sockaddr))
		return (FALSE);
	if (!build_reverse_inaddr(
	    (const struct sockaddr *)CFDataGetBytePtr(p->ptr_address),
	    reverse, sizeof(reverse)))
		return (FALSE);

	err = DNSServiceQueryRecord(&p->sd_ref,
	    kDNSServiceFlagsForceMulticast | kDNSServiceFlagsTimeout,
	    0 /* any interface */, reverse,
	    kDNSServiceType_PTR, kDNSServiceClass_IN,
	    ptr_cb, p);
	if (err != kDNSServiceErr_NoError) {
		p->error_num = (int)err;
		return (FALSE);
	}
	fd = DNSServiceRefSockFD(p->sd_ref);
	if (fd < 0) {
		DNSServiceRefDeallocate(p->sd_ref);
		p->sd_ref = NULL;
		return (FALSE);
	}

	dispatch_retain(queue);
	p->queue = queue;
	p->sd_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
	    (uintptr_t)fd, 0, queue);
	if (p->sd_source == NULL) {
		__SCNRStop(p);
		return (FALSE);
	}
	dispatch_source_set_event_handler(p->sd_source, ^{
		if (p->sd_ref != NULL)
			(void)DNSServiceProcessResult(p->sd_ref);
	});
	dispatch_activate(p->sd_source);
	return (TRUE);
}

/* Public API ---------------------------------------------------- */

SCNetworkReachabilityRef
SCNetworkReachabilityCreateWithOptions(CFAllocatorRef allocator,
    CFDictionaryRef options)
{
	SCNetworkReachabilityPrivateRef p;
	CFDataRef addr;
	CFIndex extra;

	if (options == NULL)
		return (NULL);
	addr = (CFDataRef)CFDictionaryGetValue(options,
	    kSCNetworkReachabilityOptionPTRAddress);
	if (addr == NULL || CFGetTypeID(addr) != CFDataGetTypeID())
		return (NULL);

	extra = sizeof(SCNetworkReachabilityPrivate) - sizeof(CFRuntimeBase);
	p = (SCNetworkReachabilityPrivateRef)
	    _CFRuntimeCreateInstance(allocator,
		SCNetworkReachabilityGetTypeID(), extra, NULL);
	if (p == NULL)
		return (NULL);
	memset(((char *)p) + sizeof(CFRuntimeBase), 0,
	    sizeof(SCNetworkReachabilityPrivate) - sizeof(CFRuntimeBase));
	p->ptr_address = (CFDataRef)CFRetain(addr);
	return ((SCNetworkReachabilityRef)p);
}

Boolean
SCNetworkReachabilitySetCallback(SCNetworkReachabilityRef target,
    SCNetworkReachabilityCallBack callout,
    SCNetworkReachabilityContext *context)
{
	SCNetworkReachabilityPrivateRef p = (SCNetworkReachabilityPrivateRef)target;

	if (p == NULL ||
	    CFGetTypeID(target) != SCNetworkReachabilityGetTypeID())
		return (FALSE);
	if (p->cb_ctx.release != NULL && p->cb_ctx.info != NULL)
		p->cb_ctx.release(p->cb_ctx.info);
	p->cb = callout;
	memset(&p->cb_ctx, 0, sizeof(p->cb_ctx));
	if (context != NULL) {
		p->cb_ctx = *context;
		if (p->cb_ctx.retain != NULL && p->cb_ctx.info != NULL)
			p->cb_ctx.info = (void *)p->cb_ctx.retain(p->cb_ctx.info);
	}
	return (TRUE);
}

Boolean
SCNetworkReachabilitySetDispatchQueue(SCNetworkReachabilityRef target,
    dispatch_queue_t queue)
{
	SCNetworkReachabilityPrivateRef p = (SCNetworkReachabilityPrivateRef)target;

	if (p == NULL ||
	    CFGetTypeID(target) != SCNetworkReachabilityGetTypeID())
		return (FALSE);
	if (queue == NULL) {
		__SCNRStop(p);
		return (TRUE);
	}
	if (p->sd_ref != NULL)	/* already scheduled */
		return (FALSE);
	return (__SCNRStart(p, queue));
}

CFArrayRef
SCNetworkReachabilityCopyResolvedAddress(SCNetworkReachabilityRef target,
    int *error_num)
{
	SCNetworkReachabilityPrivateRef p = (SCNetworkReachabilityPrivateRef)target;

	if (p == NULL ||
	    CFGetTypeID(target) != SCNetworkReachabilityGetTypeID())
		return (NULL);
	if (error_num != NULL)
		*error_num = p->error_num;
	if (p->resolved == NULL)
		return (NULL);
	return ((CFArrayRef)CFArrayCreateCopy(NULL, p->resolved));
}
