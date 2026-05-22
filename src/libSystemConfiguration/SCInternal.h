/*
 * SCInternal.h — private definitions shared by the libSystemConfiguration
 * sources (freebsd-launchd-mach port of Apple's SystemConfiguration.fproj
 * SCDynamicStoreInternal.h + SCD.h). Not installed.
 *
 * Include order matters: "config.h" (the MIG-generated config.defs
 * client header) pulls configd's config_types.h, which #defines the
 * kSCStatus* codes and the xmlData / xmlDataOut wire types. Pulling it
 * first means the public <SystemConfiguration/SCDynamicStore.h> sees
 * _CONFIG_TYPES_H already defined and skips its own kSCStatus enum, so
 * the two definitions never clash.
 */

#ifndef _SC_INTERNAL_H
#define _SC_INTERNAL_H

#include "config.h"		/* MIG-generated config.defs client stubs */

#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFRuntime.h>
#include <mach/mach.h>
#include <dispatch/dispatch.h>
#include <pthread.h>

#include <SystemConfiguration/SCDynamicStore.h>

/*
 * kSCStatusNoStoreServer — configd not (or no longer) reachable. A
 * client-side-only condition (a failed bootstrap lookup), so configd's
 * config_types.h does not define it, and when that header is in scope
 * the public SCDynamicStore.h skips its kSCStatus enum. Define it here
 * so the library sources can name it either way.
 */
#ifndef kSCStatusNoStoreServer
#define kSCStatusNoStoreServer	2002
#endif

/* Notification delivery status (SCNotify.c). */
enum {
	NotifierNotRegistered = 0,
	Using_NotifierInformViaDispatch,
	Using_NotifierInformViaRunLoop
};

/*
 * The SCDynamicStore session object. A CoreFoundation runtime type:
 * the leading CFRuntimeBase is what _CFRuntimeCreateInstance() fills
 * in, and CFRetain() / CFRelease() drive its lifetime.
 */
typedef struct __SCDynamicStore {
	CFRuntimeBase		cfBase;

	/* client side of the configd session */
	CFStringRef		name;		/* caller's session label */
	CFDictionaryRef		options;	/* session options, or NULL */

	/* server side: the per-session Mach port returned by configopen */
	mach_port_t		server;

	/*
	 * Notification callout + context, set at create time. The callout
	 * fires (on the dispatch queue) when a watched key changes.
	 */
	SCDynamicStoreCallBack	rlsFunction;
	SCDynamicStoreContext	rlsContext;

	/*
	 * Notification delivery state (SCNotify.c). configd notifies the
	 * session by sending a bare Mach message to notifyPort; a private
	 * thread runs a raw mach_msg receive loop on it (a dispatch
	 * DISPATCH_SOURCE_TYPE_MACH_RECV source does not reliably deliver
	 * in this repo — task #41 — so hwregd and this both use a thread).
	 * The callout still runs on the caller's dispatchQueue.
	 */
	int			notifyStatus;	/* Notifier* enum above */
	mach_port_t		notifyPort;	/* receive right configd notifies */
	pthread_t		notifyThread;	/* raw mach_msg receive loop */
	volatile int		notifyStop;	/* asks notifyThread to exit */

	/* SCDynamicStoreSetDispatchQueue delivery */
	dispatch_queue_t	dispatchQueue;	/* caller's callout queue (retained) */

	/* SCDynamicStoreCreateRunLoopSource delivery */
	CFRunLoopSourceRef	rls;		/* the v0 source (store-owned) */
	CFRunLoopRef		rlsRunLoop;	/* run loop to wake on a change */
	int			rlsScheduled;	/* CFRunLoopAddSource refcount */
} SCDynamicStorePrivate, *SCDynamicStorePrivateRef;

/* TRUE iff obj is a live SCDynamicStore. */
static inline Boolean
isA_SCDynamicStore(SCDynamicStoreRef store)
{
	return ((store != NULL) &&
		(CFGetTypeID(store) == SCDynamicStoreGetTypeID()));
}

/* SCD.c — per-thread SCError() state. */
void	_SCErrorSet(int error);

/*
 * SCNotify.c — notify-port setup and teardown, shared with the object
 * finalizer. __SCDynamicStoreAddNotificationPort allocates a receive
 * right and registers it with configd (notifyviaport); it returns the
 * port or MACH_PORT_NULL with SCError() set. __SCDynamicStoreNotify
 * Cancel tears an active dispatch-queue notification back down.
 */
mach_port_t	__SCDynamicStoreAddNotificationPort(SCDynamicStoreRef store);
void		__SCDynamicStoreNotifyCancel(SCDynamicStoreRef store);

/*
 * SCD.c — property-list <-> wire-byte serialization. configd stores
 * keys as raw UTF-8 and values as opaque serialized-plist blobs; these
 * helpers produce / parse exactly those byte forms.
 *
 *   _SCSerialize        CFPropertyList -> CFData (XML plist bytes)
 *   _SCSerializeString  CFString       -> CFData (UTF-8 bytes)
 *   _SCUnserialize      wire bytes     -> CFPropertyList
 *
 * The _SCSerialize* helpers return a retained CFData (caller releases)
 * or NULL on failure; _SCUnserialize returns a retained CFType or NULL.
 */
CFDataRef		_SCSerialize(CFPropertyListRef obj);
CFDataRef		_SCSerializeString(CFStringRef str);
CFPropertyListRef	_SCUnserialize(const void *bytes, CFIndex len);

/*
 * SCD.c — encode a CFArray of CFString keys into configd's wire
 * key-list form ([uint32 LE len][bytes] records). Returns 0 with the
 * byte count in *outLen, or -1 on a bad element / buffer overflow.
 * Shared by the notification watch-set and the multi-get key list.
 */
int	_SCEncodeKeyList(CFArrayRef array, uint8_t *buf, size_t cap,
			 size_t *outLen);

/* configd's inline config.defs payload bound (config_types.h). */
#ifndef CONFIG_DATA_MAX
#define CONFIG_DATA_MAX		8192
#endif

#endif	/* _SC_INTERNAL_H */
