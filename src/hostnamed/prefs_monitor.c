/*
 * prefs_monitor.c — PreferencesMonitor-equivalent for hostnamed.
 *
 * Real-macOS analog: PreferencesMonitor is the configd plugin that
 * watches the system network-preferences file
 * (/Library/Preferences/SystemConfiguration/preferences.plist) and,
 * on every committed change, republishes the relevant subset into
 * SCDynamicStore — including Setup:/System/ComputerName and
 * Setup:/Network/HostNames. Apple's set-hostname.c (vendored under
 * src/hostnamed/vendored/) then reads those SCDS keys via
 * SCDynamicStoreKeyCreateComputerName / KeyCreateHostNames.
 *
 * Our build has no PreferencesMonitor plugin — configd is a thin
 * SCDynamicStore server, not a plugin host. We provide the same
 * SCPrefs → SCDS bridge inside hostnamed itself: ~100 LOC. On
 * startup we publish initial values (synthesized fallback when the
 * prefs file is absent / has no ComputerName); on every
 * SCPreferencesCommitChanges from another process (e.g. the
 * hostnameprefset CI fixture) the SCPrefs callback re-reads the
 * prefs and re-publishes.
 *
 * Boot ordering: prefs_monitor's initial publish runs FIRST in
 * hostnamed's main(), before load_hostname() — so by the time
 * set-hostname.c's decision engine wakes up, Setup:/System and
 * Setup:/Network/HostNames are already populated.
 */

#include "freebsd-shim/ip_plugin.h"

#include <SystemConfiguration/SCDynamicStore.h>
#include <SystemConfiguration/SCPreferences.h>
#include <SystemConfiguration/SCSchemaDefinitions.h>

#include <CoreFoundation/CoreFoundation.h>

#include <dispatch/dispatch.h>

#include <stdio.h>
#include <string.h>

/* hostnamed.c's logger. */
extern void	xlog(const char *fmt, ...);

/* Internal state — singleton per process. */
struct prefs_monitor_state {
	SCPreferencesRef	prefs;
	SCDynamicStoreRef	store;
	dispatch_queue_t	queue;
};
static struct prefs_monitor_state g_state;

/* Read ComputerName out of /System/System path; NULL if absent or
 * unreadable. Caller releases. */
static CFStringRef
read_prefs_computer_name(SCPreferencesRef prefs)
{
	CFStringRef path, name = NULL;
	CFDictionaryRef sysdict;

	path = CFStringCreateWithFormat(NULL, NULL, CFSTR("/%@/%@"),
	    kSCCompSystem, kSCCompSystem);
	if (path == NULL)
		return (NULL);
	sysdict = SCPreferencesPathGetValue(prefs, path);
	CFRelease(path);
	if (sysdict == NULL ||
	    CFGetTypeID(sysdict) != CFDictionaryGetTypeID())
		return (NULL);
	{
		CFStringRef v = CFDictionaryGetValue(sysdict,
		    kSCPropSystemComputerName);
		if (v != NULL && CFGetTypeID(v) == CFStringGetTypeID())
			name = CFStringCreateCopy(NULL, v);
	}
	return (name);
}

/* Compute the name we want to publish: SCPrefs ComputerName if set,
 * otherwise synthesized slug+suffix fallback. Caller releases. */
static CFStringRef
compute_publish_name(SCPreferencesRef prefs)
{
	CFStringRef name = read_prefs_computer_name(prefs);
	if (name != NULL)
		return (name);
	return (freebsd_synthesize_hostname());
}

/* Build and publish Setup:/System (ComputerName + ComputerNameEncoding=
 * UTF-8) and Setup:/Network/HostNames (HostName + LocalHostName, same
 * value). */
static void
publish(SCDynamicStoreRef store, CFStringRef name)
{
	CFStringRef key;
	CFMutableDictionaryRef dict;
	CFNumberRef enc_num;
	SInt32 enc_val = (SInt32)kCFStringEncodingUTF8;

	if (store == NULL || name == NULL)
		return;

	/* Setup:/System — both ComputerName (user-visible) AND HostName
	 * (DNS-safe form). set-hostname.c's copy_prefs_hostname reads
	 * the HostName key out of this dict; without it the engine
	 * never sees the SCPrefs tier and falls all the way to mDNS /
	 * localhost. For our build the two values are the same — the
	 * synthesized slug+suffix is already DNS-safe. ComputerNameEncoding
	 * is informational (UTF-8). */
	key = SCDynamicStoreKeyCreateComputerName(NULL);
	dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	enc_num = CFNumberCreate(NULL, kCFNumberSInt32Type, &enc_val);
	if (key != NULL && dict != NULL && enc_num != NULL) {
		CFDictionarySetValue(dict, kSCPropSystemComputerName, name);
		CFDictionarySetValue(dict, kSCPropSystemHostName, name);
		CFDictionarySetValue(dict,
		    kSCPropSystemComputerNameEncoding, enc_num);
		if (!SCDynamicStoreSetValue(store, key, dict))
			xlog("prefs_monitor: SetValue(Setup:/System) failed");
	}
	if (enc_num != NULL) CFRelease(enc_num);
	if (dict != NULL) CFRelease(dict);
	if (key != NULL) CFRelease(key);

	/* Setup:/Network/HostNames */
	key = SCDynamicStoreKeyCreateHostNames(NULL);
	dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	if (key != NULL && dict != NULL) {
		CFDictionarySetValue(dict, kSCPropNetHostName, name);
		CFDictionarySetValue(dict, kSCPropNetLocalHostName, name);
		if (!SCDynamicStoreSetValue(store, key, dict))
			xlog("prefs_monitor: SetValue("
			    "Setup:/Network/HostNames) failed");
	}
	if (dict != NULL) CFRelease(dict);
	if (key != NULL) CFRelease(key);
}

/* SCPrefs commit callback: re-read SCPrefs (which may have just been
 * mutated by hostnameprefset), re-publish to SCDS. set-hostname.c's
 * SCDS callback then fires for the Setup:/System change and re-runs
 * the decision engine. */
static void
prefs_cb(SCPreferencesRef prefs, SCPreferencesNotification type,
    void *info)
{
	CFStringRef name;
	struct prefs_monitor_state *st = info;

	(void)type;
	if (st == NULL || st->store == NULL)
		return;
	/* Note: Apple's SCPreferencesSynchronize would normally drop the
	 * in-memory cache so the next read picks up the just-committed
	 * value; our libSC port doesn't ship it yet. SCPreferencesPath
	 * GetValue against the in-tree port re-reads from the on-disk
	 * plist on each call (no cache), so the synchronize is currently
	 * a no-op. If that changes (libSC adds caching), add the sync
	 * back and ship a SCPreferencesSynchronize stub in libSC. */
	name = compute_publish_name(prefs);
	if (name == NULL)
		return;
	{
		char buf[256];
		if (CFStringGetCString(name, buf, sizeof(buf),
		    kCFStringEncodingUTF8))
			xlog("prefs_monitor: re-publishing '%s'", buf);
	}
	publish(st->store, name);
	CFRelease(name);
}

/* Entry: open SCPrefs + SCDS, register the SCPrefs callback on the
 * supplied dispatch queue, do the initial publish. Returns 0 on
 * success; non-zero on a failure that prevents publishing. The
 * caller (hostnamed's main) keeps `queue` alive for the daemon's
 * lifetime. */
int
prefs_monitor_start(dispatch_queue_t queue)
{
	SCPreferencesContext pctx;
	CFStringRef name;

	memset(&g_state, 0, sizeof(g_state));
	g_state.queue = queue;
	g_state.prefs = SCPreferencesCreate(NULL,
	    CFSTR("com.apple.hostnamed.prefs"), CFSTR("preferences.plist"));
	if (g_state.prefs == NULL) {
		xlog("prefs_monitor: SCPreferencesCreate failed");
		return (-1);
	}
	g_state.store = SCDynamicStoreCreate(NULL,
	    CFSTR("com.apple.hostnamed.publish"), NULL, NULL);
	if (g_state.store == NULL) {
		xlog("prefs_monitor: SCDynamicStoreCreate failed");
		return (-1);
	}

	/* Initial publish FIRST (synthesized fallback if needed). */
	name = compute_publish_name(g_state.prefs);
	if (name == NULL) {
		xlog("prefs_monitor: compute_publish_name returned NULL");
		return (-1);
	}
	{
		char buf[256];
		if (CFStringGetCString(name, buf, sizeof(buf),
		    kCFStringEncodingUTF8))
			xlog("prefs_monitor: initial publish '%s'", buf);
	}
	publish(g_state.store, name);
	CFRelease(name);

	/* Register the callback for ongoing changes. */
	memset(&pctx, 0, sizeof(pctx));
	pctx.info = &g_state;
	if (!SCPreferencesSetCallback(g_state.prefs, prefs_cb, &pctx)) {
		xlog("prefs_monitor: SCPreferencesSetCallback failed");
		return (-1);
	}
	if (!SCPreferencesSetDispatchQueue(g_state.prefs, queue)) {
		xlog("prefs_monitor: SCPreferencesSetDispatchQueue failed");
		return (-1);
	}
	xlog("prefs_monitor: watching /System/System/ComputerName via SCPrefs");
	return (0);
}
