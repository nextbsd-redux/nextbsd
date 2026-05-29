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

/* Returns the SCPrefs ComputerName if set, otherwise NULL. The
 * caller distinguishes "publish this value" from "unpublish so the
 * engine falls through to DHCP / PTR / mDNS / freebsd_synthesize
 * (the carry) on its own." Previously this returned the synth
 * fallback unconditionally — which made set-hostname.c's SCPrefs
 * tier always win, masking DHCP / mDNS / PTR. */
static CFStringRef
compute_publish_name(SCPreferencesRef prefs)
{
	return (read_prefs_computer_name(prefs));
}

/* Publish (name non-NULL) or unpublish (name NULL) Setup:/System.
 * Unpublish lets set-hostname.c's copy_prefs_hostname return NULL so
 * the engine falls through to DHCP / PTR / mDNS / synth carry.
 *
 * Both kSCPropSystemComputerName (user-visible) and kSCPropSystemHostName
 * (DNS-safe) carry the same value here; the synthesised slug+suffix is
 * already DNS-safe so no second derivation is needed. ComputerNameEncoding
 * is informational (UTF-8). */
static void
publish_setup_system(SCDynamicStoreRef store, CFStringRef name)
{
	CFStringRef key;
	CFMutableDictionaryRef dict;
	CFNumberRef enc_num;
	SInt32 enc_val = (SInt32)kCFStringEncodingUTF8;

	if (store == NULL)
		return;
	key = SCDynamicStoreKeyCreateComputerName(NULL);
	if (key == NULL)
		return;
	if (name == NULL) {
		(void)SCDynamicStoreRemoveValue(store, key);
		CFRelease(key);
		return;
	}
	dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	enc_num = CFNumberCreate(NULL, kCFNumberSInt32Type, &enc_val);
	if (dict != NULL && enc_num != NULL) {
		CFDictionarySetValue(dict, kSCPropSystemComputerName, name);
		CFDictionarySetValue(dict, kSCPropSystemHostName, name);
		CFDictionarySetValue(dict,
		    kSCPropSystemComputerNameEncoding, enc_num);
		if (!SCDynamicStoreSetValue(store, key, dict))
			xlog("prefs_monitor: SetValue(Setup:/System) failed");
	}
	if (enc_num != NULL) CFRelease(enc_num);
	if (dict != NULL) CFRelease(dict);
	CFRelease(key);
}

/* Publish (name non-NULL) or unpublish (name NULL) Setup:/Network/HostNames.
 * The HostNames key carries kSCPropNetHostName + kSCPropNetLocalHostName —
 * mDNSResponder reads LocalHostName for the .local broadcast (see
 * apple-oss-distributions/mDNSResponder mDNSMacOSX.c:3404
 * GetUserSpecifiedLocalHostName). When SCPrefs is empty we want this
 * key POPULATED (with the synthesised name) even though Setup:/System
 * is empty — that's the freebsd-launchd-mach split:
 *
 *   - Setup:/System mirrors SCPrefs ComputerName, so absent = empty
 *     (lets set-hostname.c's decision engine fall through to DHCP /
 *     PTR / mDNS tiers in the test rounds and on real first boots).
 *   - Setup:/Network/HostNames carries a usable .local name regardless,
 *     either the SCPrefs value when set, or the synthesised slug+suffix
 *     otherwise. mDNSResponder gets a real name to announce on a fresh
 *     machine instead of "Amnesiac".
 */
static void
publish_hostnames(SCDynamicStoreRef store, CFStringRef name)
{
	CFStringRef key;
	CFMutableDictionaryRef dict;

	if (store == NULL)
		return;
	key = SCDynamicStoreKeyCreateHostNames(NULL);
	if (key == NULL)
		return;
	if (name == NULL) {
		(void)SCDynamicStoreRemoveValue(store, key);
		CFRelease(key);
		return;
	}
	dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	if (dict != NULL) {
		CFDictionarySetValue(dict, kSCPropNetHostName, name);
		CFDictionarySetValue(dict, kSCPropNetLocalHostName, name);
		if (!SCDynamicStoreSetValue(store, key, dict))
			xlog("prefs_monitor: SetValue("
			    "Setup:/Network/HostNames) failed");
		CFRelease(dict);
	}
	CFRelease(key);
}

/* Apply the current state: if SCPrefs ComputerName is set, mirror it
 * into BOTH Setup:/System and Setup:/Network/HostNames. If absent,
 * unpublish Setup:/System (engine falls through) but publish the
 * synthesised name to Setup:/Network/HostNames so mDNSResponder has
 * a real .local broadcast name on a fresh machine. */
static void
apply(SCDynamicStoreRef store, CFStringRef scprefs_name)
{
	if (store == NULL)
		return;
	if (scprefs_name != NULL) {
		publish_setup_system(store, scprefs_name);
		publish_hostnames(store, scprefs_name);
		return;
	}
	publish_setup_system(store, NULL);
	{
		CFStringRef synth = freebsd_synthesize_hostname();
		if (synth != NULL) {
			publish_hostnames(store, synth);
			CFRelease(synth);
		} else {
			publish_hostnames(store, NULL);
		}
	}
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
	/* Drop the cached in-memory plist so the next PathGetValue picks
	 * up the value just committed by another process (e.g. the
	 * hostnameprefset CI fixture). Without this, our SCPrefs ref
	 * stays at the snapshot loaded at SCPreferencesCreate time. */
	SCPreferencesSynchronize(prefs);
	name = compute_publish_name(prefs);
	if (name == NULL) {
		xlog("prefs_monitor: SCPrefs ComputerName absent — "
		    "Setup:/System empty (engine falls through); "
		    "Setup:/Network/HostNames = synthesised");
		apply(st->store, NULL);
		return;
	}
	{
		char buf[256];
		if (CFStringGetCString(name, buf, sizeof(buf),
		    kCFStringEncodingUTF8))
			xlog("prefs_monitor: re-publishing '%s' "
			    "(both Setup:/System and Setup:/Network/HostNames)",
			    buf);
	}
	apply(st->store, name);
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

	/* Initial publish. SCPrefs absent → Setup:/System empty (engine
	 * falls through to DHCP / PTR / mDNS), Setup:/Network/HostNames
	 * carries the synthesised slug+suffix so mDNSResponder broadcasts
	 * a real .local name on a fresh machine. SCPrefs set → both keys
	 * carry that user value (Apple-shape mirror). */
	name = compute_publish_name(g_state.prefs);
	if (name == NULL) {
		xlog("prefs_monitor: SCPrefs ComputerName absent at boot — "
		    "Setup:/System empty (engine falls through); "
		    "Setup:/Network/HostNames will carry synthesised name "
		    "(mDNSResponder broadcasts <synth>.local)");
		apply(g_state.store, NULL);
	} else {
		char buf[256];
		if (CFStringGetCString(name, buf, sizeof(buf),
		    kCFStringEncodingUTF8))
			xlog("prefs_monitor: initial publish '%s' "
			    "(both Setup:/System and Setup:/Network/HostNames)",
			    buf);
		apply(g_state.store, name);
		CFRelease(name);
	}

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
