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
	 * Notification callout + context. Stored at create time; unused
	 * until the notification iteration. Kept here so adding
	 * notifications does not change the object layout.
	 */
	SCDynamicStoreCallBack	rlsFunction;
	SCDynamicStoreContext	rlsContext;
} SCDynamicStorePrivate, *SCDynamicStorePrivateRef;

/* SCD.c — per-thread SCError() state. */
void	_SCErrorSet(int error);

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

/* configd's inline config.defs payload bound (config_types.h). */
#ifndef CONFIG_DATA_MAX
#define CONFIG_DATA_MAX		8192
#endif

#endif	/* _SC_INTERNAL_H */
