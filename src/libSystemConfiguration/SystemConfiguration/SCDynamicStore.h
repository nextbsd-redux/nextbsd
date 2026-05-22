/*
 * Copyright (c) 2000-2022 Apple Inc. All rights reserved.
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
 * SCDynamicStore.h — the SCDynamicStore client API.
 *
 * freebsd-launchd-mach port: a trimmed port of Apple's
 * SystemConfiguration.framework SCDynamicStore.h. The SCDynamicStore
 * is configd's system-wide key -> property-list store; this header is
 * what a client links against (-lSystemConfiguration) to talk to it.
 *
 * iter 1 covers the synchronous store API — create a session, then
 * get / set / add / remove values and list keys. Change notifications
 * (SCDynamicStoreSetNotificationKeys / SCDynamicStoreCreateRunLoopSource
 * / the SCDynamicStoreCallBack) are a later iteration; the callback +
 * context parameters of SCDynamicStoreCreate are accepted and stored
 * now so the ABI does not move when notifications land.
 */

#ifndef _SCDYNAMICSTORE_H
#define _SCDYNAMICSTORE_H

#include <sys/cdefs.h>
#include <dispatch/dispatch.h>
#include <CoreFoundation/CoreFoundation.h>

/*!
	@typedef SCDynamicStoreRef
	@discussion This is the handle to an open "session" with the
		dynamic store.  It is a CoreFoundation type (see
		SCDynamicStoreGetTypeID()) — release it with CFRelease().
 */
typedef const struct __SCDynamicStore *	SCDynamicStoreRef;

/*!
	@typedef SCDynamicStoreContext
	@discussion Structure containing client-supplied data and callbacks
		associated with an SCDynamicStore session.
 */
typedef struct {
	CFIndex		version;
	void *		info;
	const void *	(*retain)(const void *info);
	void		(*release)(const void *info);
	CFStringRef	(*copyDescription)(const void *info);
} SCDynamicStoreContext;

/*!
	@typedef SCDynamicStoreCallBack
	@discussion Invoked when a watched key/pattern changes.  Unused
		until the notification iteration; declared here so the
		SCDynamicStoreCreate() signature is stable.
 */
typedef void (*SCDynamicStoreCallBack)(SCDynamicStoreRef	store,
				       CFArrayRef		changedKeys,
				       void *			info);

/*
 * SCDynamicStore status codes — SCError() return values.  These mirror
 * configd's config_types.h; when this header is pulled into a configd /
 * libSystemConfiguration translation unit that already saw that file
 * (it #defines the same names) the enum is skipped to avoid a clash.
 */
#ifndef _CONFIG_TYPES_H
enum {
	kSCStatusOK			= 0,	/* success */
	kSCStatusFailed			= 1001,	/* non-specific failure */
	kSCStatusInvalidArgument	= 1002,	/* invalid argument */
	kSCStatusNoKey			= 1004,	/* no such key */
	kSCStatusKeyExists		= 1005,	/* key already defined */
	kSCStatusNoStoreSession		= 2001,	/* no open store session */
	kSCStatusNoStoreServer		= 2002,	/* configd not (no longer) available */
	kSCStatusNotifierActive		= 2003,	/* notifier already active */
	kSCStatusNoPrefsSession		= 5001,	/* no open preferences session */
	kSCStatusPrefsBusy		= 5002,	/* preferences update in progress */
	kSCStatusNoConfigFile		= 5003,	/* no configuration file */
	kSCStatusStale			= 5005,	/* write attempted on stale prefs */
	kSCStatusAccessError		= 5006,	/* permission/authorization failure */
	kSCStatusLocked			= 5007,	/* preferences already locked */
	kSCStatusNeedLock		= 5008	/* preferences not locked */
};
#endif	/* !_CONFIG_TYPES_H */

__BEGIN_DECLS

/*!
	@function SCDynamicStoreGetTypeID
	@discussion Returns the CoreFoundation type identifier of all
		SCDynamicStore instances.
 */
CFTypeID
SCDynamicStoreGetTypeID		(void);

/*!
	@function SCDynamicStoreCreate
	@discussion Opens a session with the dynamic store maintained by
		configd.
	@param allocator   the CFAllocator to use, or NULL.
	@param name        a label identifying the calling process.
	@param callout     change-notification callback (unused in iter 1).
	@param context     callback context (unused in iter 1).
	@result a new session handle, or NULL on error (see SCError()).
 */
SCDynamicStoreRef
SCDynamicStoreCreate		(CFAllocatorRef			allocator,
				 CFStringRef			name,
				 SCDynamicStoreCallBack		callout,
				 SCDynamicStoreContext		*context);

/*!
	@function SCDynamicStoreCreateWithOptions
	@discussion As SCDynamicStoreCreate(), with an extra options
		dictionary handed to configd at session open.
 */
SCDynamicStoreRef
SCDynamicStoreCreateWithOptions	(CFAllocatorRef			allocator,
				 CFStringRef			name,
				 CFDictionaryRef		storeOptions,
				 SCDynamicStoreCallBack		callout,
				 SCDynamicStoreContext		*context);

/*!
	@function SCDynamicStoreCopyKeyList
	@discussion Returns the keys in the dynamic store that match the
		supplied POSIX regular-expression pattern.
	@result a CFArray of CFString keys (caller releases), or NULL.
 */
CFArrayRef
SCDynamicStoreCopyKeyList	(SCDynamicStoreRef		store,
				 CFStringRef			pattern);

/*!
	@function SCDynamicStoreCopyMultiple
	@discussion Fetches the values for several keys in one call.
	@param keys     a CFArray of CFString keys to fetch, or NULL.
	@param patterns a CFArray of CFString POSIX-regex patterns whose
		matching keys are also fetched, or NULL.
	@result a CFDictionary of key -> value (caller releases), or NULL.
 */
CFDictionaryRef
SCDynamicStoreCopyMultiple	(SCDynamicStoreRef		store,
				 CFArrayRef			keys,
				 CFArrayRef			patterns);

/*!
	@function SCDynamicStoreSetMultiple
	@discussion Sets, removes and posts notifications for several keys
		in one call.
	@param keysToSet     a CFDictionary of key -> value to set, or NULL.
	@param keysToRemove  a CFArray of CFString keys to remove, or NULL.
	@param keysToNotify  a CFArray of CFString keys to flag changed
		without altering their value, or NULL.
 */
Boolean
SCDynamicStoreSetMultiple	(SCDynamicStoreRef		store,
				 CFDictionaryRef		keysToSet,
				 CFArrayRef			keysToRemove,
				 CFArrayRef			keysToNotify);

/*!
	@function SCDynamicStoreCopyValue
	@discussion Fetches the value associated with a key.
	@result the value (caller releases), or NULL if no such key.
 */
CFPropertyListRef
SCDynamicStoreCopyValue		(SCDynamicStoreRef		store,
				 CFStringRef			key);

/*!
	@function SCDynamicStoreAddValue
	@discussion Adds a key/value to the store; fails if the key
		already exists.
 */
Boolean
SCDynamicStoreAddValue		(SCDynamicStoreRef		store,
				 CFStringRef			key,
				 CFPropertyListRef		value);

/*!
	@function SCDynamicStoreSetValue
	@discussion Sets the value associated with a key, creating the
		key if needed.
 */
Boolean
SCDynamicStoreSetValue		(SCDynamicStoreRef		store,
				 CFStringRef			key,
				 CFPropertyListRef		value);

/*!
	@function SCDynamicStoreRemoveValue
	@discussion Removes a key (and its value) from the store.
 */
Boolean
SCDynamicStoreRemoveValue	(SCDynamicStoreRef		store,
				 CFStringRef			key);

/*!
	@function SCDynamicStoreSetNotificationKeys
	@discussion Specifies the keys and key patterns the session wants
		change notifications for. Replaces any previous set.
	@param keys     a CFArray of CFString keys to watch, or NULL.
	@param patterns a CFArray of CFString POSIX-regex patterns, or NULL.
 */
Boolean
SCDynamicStoreSetNotificationKeys (SCDynamicStoreRef		store,
				   CFArrayRef			keys,
				   CFArrayRef			patterns);

/*!
	@function SCDynamicStoreCopyNotifiedKeys
	@discussion Returns the keys that have changed since the last call,
		draining the session's pending-change list.
	@result a CFArray of CFString keys (caller releases), or NULL.
 */
CFArrayRef
SCDynamicStoreCopyNotifiedKeys	(SCDynamicStoreRef		store);

/*!
	@function SCDynamicStoreSetDispatchQueue
	@discussion Schedules change-notification delivery onto a dispatch
		queue: the SCDynamicStoreCallBack passed to SCDynamicStore
		Create() runs on `queue` whenever a watched key changes.
		Pass NULL to unschedule.
 */
Boolean
SCDynamicStoreSetDispatchQueue	(SCDynamicStoreRef		store,
				 dispatch_queue_t		queue);

/*!
	@function SCDynamicStoreCreateRunLoopSource
	@discussion Creates a CFRunLoopSource for the session. Once added
		to a run loop with CFRunLoopAddSource(), the SCDynamicStore
		CallBack passed to SCDynamicStoreCreate() runs on that run
		loop whenever a watched key changes.
	@result a CFRunLoopSource (caller releases), or NULL on error.
 */
CFRunLoopSourceRef
SCDynamicStoreCreateRunLoopSource (CFAllocatorRef		allocator,
				   SCDynamicStoreRef		store,
				   CFIndex			order);

/*!
	@function SCError
	@discussion Returns the most recent status/error code, as a
		kSCStatus* value, for the calling thread.
 */
int
SCError				(void);

/*!
	@function SCErrorString
	@discussion Returns a human-readable string for a kSCStatus* code.
 */
const char *
SCErrorString			(int				status);

__END_DECLS

#endif	/* _SCDYNAMICSTORE_H */
