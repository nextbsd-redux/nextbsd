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
 * SCPreferences.h — the SCPreferences client API.
 *
 * freebsd-launchd-mach port: a trimmed port of Apple's
 * SystemConfiguration.framework SCPreferences.h. SCPreferences is the
 * persistent counterpart of SCDynamicStore — a property-list file
 * (the system network configuration lives in one) read into memory,
 * edited, and committed back.
 *
 * iter 1 covers the synchronous read / edit / commit cycle: create a
 * session, get / set / remove values and list keys, then commit the
 * changes back to the file. The preferences lock, change notifications
 * (SCPreferencesSetCallback), SCPreferencesApplyChanges and the path-
 * based accessors are later iterations.
 */

#ifndef _SCPREFERENCES_H
#define _SCPREFERENCES_H

#include <sys/cdefs.h>
#include <dispatch/dispatch.h>
#include <CoreFoundation/CoreFoundation.h>

/*!
	@typedef SCPreferencesRef
	@discussion Handle to an open preferences session. A CoreFoundation
		type (see SCPreferencesGetTypeID()) — release with CFRelease().
 */
typedef const struct __SCPreferences *	SCPreferencesRef;

/*!
	@enum SCPreferencesNotification
	@discussion The kinds of change an SCPreferencesCallBack reports.
 */
typedef CFOptionFlags	SCPreferencesNotification;
enum {
	kSCPreferencesNotificationCommit	= 1<<0,	/* prefs committed */
	kSCPreferencesNotificationApply		= 1<<1	/* prefs should apply */
};

/*!
	@typedef SCPreferencesContext
	@discussion Client-supplied data + callbacks for an SCPreferences
		callback.
 */
typedef struct {
	CFIndex		version;
	void *		info;
	const void *	(*retain)(const void *info);
	void		(*release)(const void *info);
	CFStringRef	(*copyDescription)(const void *info);
} SCPreferencesContext;

/*!
	@typedef SCPreferencesCallBack
	@discussion Invoked when the watched preferences change.
 */
typedef void (*SCPreferencesCallBack)(SCPreferencesRef		prefs,
				      SCPreferencesNotification	notificationType,
				      void *			info);

__BEGIN_DECLS

/*!
	@function SCPreferencesGetTypeID
	@discussion Returns the CoreFoundation type identifier of all
		SCPreferences instances.
 */
CFTypeID
SCPreferencesGetTypeID		(void);

/*!
	@function SCPreferencesCreate
	@discussion Opens a session to the named preferences file.
	@param allocator the CFAllocator to use, or NULL.
	@param name      a label identifying the calling process.
	@param prefsID   the preferences file: NULL for the default
		(/Library/Preferences/SystemConfiguration/preferences.plist),
		a name resolved within that directory, or a path containing
		a '/' used as-is.
	@result a new session handle (the file is read lazily on first
		access), or NULL on error (see SCError()).
 */
SCPreferencesRef
SCPreferencesCreate		(CFAllocatorRef			allocator,
				 CFStringRef			name,
				 CFStringRef			prefsID);

/*!
	@function SCPreferencesGetValue
	@discussion Returns the value for a top-level preferences key.
		The value is owned by the session — do not release it, and
		do not use it past the next change to the session.
 */
CFPropertyListRef
SCPreferencesGetValue		(SCPreferencesRef		prefs,
				 CFStringRef			key);

/*!
	@function SCPreferencesSetValue
	@discussion Sets the value for a top-level preferences key. The
		change is in memory until SCPreferencesCommitChanges().
 */
Boolean
SCPreferencesSetValue		(SCPreferencesRef		prefs,
				 CFStringRef			key,
				 CFPropertyListRef		value);

/*!
	@function SCPreferencesRemoveValue
	@discussion Removes a top-level preferences key.
 */
Boolean
SCPreferencesRemoveValue	(SCPreferencesRef		prefs,
				 CFStringRef			key);

/*!
	@function SCPreferencesCopyKeyList
	@discussion Returns the top-level preferences keys.
	@result a CFArray of CFString keys (caller releases).
 */
CFArrayRef
SCPreferencesCopyKeyList	(SCPreferencesRef		prefs);

/*!
	@function SCPreferencesCommitChanges
	@discussion Writes any in-memory changes back to the preferences
		file.
 */
Boolean
SCPreferencesCommitChanges	(SCPreferencesRef		prefs);

/*!
	@function SCPreferencesLock
	@discussion Takes exclusive access to the preferences. With
		`wait` TRUE the call blocks until the lock is available;
		with FALSE it fails immediately (kSCStatusPrefsBusy) if the
		lock is held. SCPreferencesCommitChanges takes the lock
		itself if the caller has not.
 */
Boolean
SCPreferencesLock		(SCPreferencesRef		prefs,
				 Boolean			wait);

/*!
	@function SCPreferencesUnlock
	@discussion Releases the exclusive access taken by
		SCPreferencesLock.
 */
Boolean
SCPreferencesUnlock		(SCPreferencesRef		prefs);

/*!
	@function SCPreferencesPathGetValue
	@discussion Returns the dictionary at a '/'-separated path within
		the preferences. The dictionary is owned by the session.
 */
CFDictionaryRef
SCPreferencesPathGetValue	(SCPreferencesRef		prefs,
				 CFStringRef			path);

/*!
	@function SCPreferencesPathSetValue
	@discussion Stores a dictionary at a '/'-separated path, creating
		any intermediate dictionaries.
 */
Boolean
SCPreferencesPathSetValue	(SCPreferencesRef		prefs,
				 CFStringRef			path,
				 CFDictionaryRef		value);

/*!
	@function SCPreferencesPathRemoveValue
	@discussion Removes the dictionary at a '/'-separated path; any
		intermediate dictionaries left empty are pruned.
 */
Boolean
SCPreferencesPathRemoveValue	(SCPreferencesRef		prefs,
				 CFStringRef			path);

/*!
	@function SCPreferencesPathCreateUniqueChild
	@discussion Creates a uniquely-named empty child dictionary under
		`prefix` and returns its full '/'-separated path (caller
		releases). The child name is a fresh UUID, so a new entry
		never collides with an existing one.
 */
CFStringRef
SCPreferencesPathCreateUniqueChild
				(SCPreferencesRef		prefs,
				 CFStringRef			prefix);

/*!
	@function SCPreferencesPathGetLink
	@discussion Returns the link target if the dictionary at `path` is
		a link, or NULL. Unlike SCPreferencesPathGetValue this does
		not follow the link.
 */
CFStringRef
SCPreferencesPathGetLink	(SCPreferencesRef		prefs,
				 CFStringRef			path);

/*!
	@function SCPreferencesPathSetLink
	@discussion Stores a link at `path` pointing at `link`; subsequent
		path walks through `path` continue from `link`. The link
		target must already exist.
 */
Boolean
SCPreferencesPathSetLink	(SCPreferencesRef		prefs,
				 CFStringRef			path,
				 CFStringRef			link);

/*!
	@function SCPreferencesSetCallback
	@discussion Sets the callback invoked when the preferences change.
 */
Boolean
SCPreferencesSetCallback	(SCPreferencesRef		prefs,
				 SCPreferencesCallBack		callout,
				 SCPreferencesContext		*context);

/*!
	@function SCPreferencesSetDispatchQueue
	@discussion Schedules change-notification delivery onto a dispatch
		queue: the SCPreferencesCallBack runs on `queue` when the
		preferences are committed. Pass NULL to unschedule.
 */
Boolean
SCPreferencesSetDispatchQueue	(SCPreferencesRef		prefs,
				 dispatch_queue_t		queue);

/*!
	@function _SCPreferencesCopyComputerName
	@discussion Reads the user-set ComputerName from the prefs file
		(stored at path /System/System). Returns CFString or NULL.
		If `nameEncoding` is non-NULL, also writes the stored
		ComputerNameEncoding (kCFStringEncodingUTF8 by default).
		Apple's SystemConfiguration.fproj exposes this as an "_SC"
		SPI; vendored consumers like set-hostname.c call it directly.
		Caller releases.
 */
CFStringRef
_SCPreferencesCopyComputerName	(SCPreferencesRef		prefs,
				 CFStringEncoding *		nameEncoding);

__END_DECLS

#endif	/* _SCPREFERENCES_H */
