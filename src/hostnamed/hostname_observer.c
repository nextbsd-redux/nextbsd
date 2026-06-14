/*
 * hostname_observer.c — Bonjour conflict-rename feedback for hostnamed.
 * freebsd-launchd-mach iter 5, issue #156.
 *
 * THE LOOP THIS CLOSES
 *
 * mDNSResponder owns the dot-local host name on the link. When it probes
 * <name>.local and finds a collision, mDNSCore's conflict-rename
 * (mDNS_HostNameCallback -> IncrementLabelSuffix) bumps the label to
 * "<name>-2", "<name>-3", ... Through iter 4 that resolved name lived only
 * in mDNSResponder's memory; nothing persisted it, so the next boot would
 * collide again. (Apple's PosixDaemon.c left exactly this as an empty stub
 * with a comment: "On Mac OS X we store the current dot-local mDNS host
 * name in the SCPreferences store.")
 *
 * iter 5 fills that stub: mDNSResponder now writes the resolved label to
 * State:/Network/HostNames (the SCDynamicStore "actual name in use"
 * surface — see src/mDNSResponder/mDNSPosix/mDNSConfigStore.c
 * mDNSConfigStorePublishResolvedHostName). This observer subscribes to that
 * State: key and, on a resolved-name change, persists it into SCPreferences
 * /System/System ComputerName + commits — the same write the hostnameprefset
 * CI fixture performs. prefs_monitor's SCPrefs callback then republishes
 * Setup:/System + Setup:/Network/HostNames, and the vendored set-hostname.c
 * decision engine sets the kernel hostname. Convergence:
 *
 *   mDNS conflict -> State:/Network/HostNames = "foo-2"
 *     -> [this observer] SCPrefs ComputerName = "foo-2" + commit
 *     -> prefs_monitor republishes Setup:/{System,Network/HostNames} = "foo-2"
 *     -> mDNSConfigStore re-adopts desired "foo-2", re-probes foo-2.local
 *        (now unique) -> no further rename -> no further State: write.
 *
 * NO-LOOP. We watch only State:/Network/HostNames. prefs_monitor writes
 * Setup: (not State:), and mDNSResponder rewrites State: only on an actual
 * conflict — which won't recur once the unique "-2" name is adopted. The
 * compare-before-write guard (resolved == current ComputerName -> skip) is
 * a belt-and-suspenders second line of defence.
 *
 * Apple-divergence note: #156 specifies persisting into ComputerName. Real
 * macOS only suffixes LocalHostName and leaves the user's ComputerName
 * untouched. Targeting ComputerName here is a deliberate choice for the
 * headless server port (ComputerName is the single name source the rest of
 * the chain keys off); flip the path string below to a LocalHostName pref
 * if Apple-exact semantics are wanted later.
 */

#include <SystemConfiguration/SCDynamicStore.h>
#include <SystemConfiguration/SCPreferences.h>
#include <SystemConfiguration/SCSchemaDefinitions.h>

#include <CoreFoundation/CoreFoundation.h>

#include <dispatch/dispatch.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>		/* sethostname */

/* hostnamed.c's logger. */
extern void	xlog(const char *fmt, ...);

/* Internal state — singleton per process. */
struct observer_state {
	SCDynamicStoreRef	store;
	dispatch_queue_t	queue;
};
static struct observer_state g_state;

/* Read the conflict-resolved LocalHostName from State:/Network/HostNames —
 * the key mDNSResponder writes after a Bonjour conflict-rename. NULL if the
 * key/value is absent (the common case: no conflict has occurred). Caller
 * releases. */
static CFStringRef
read_state_local_hostname(SCDynamicStoreRef store)
{
	CFStringRef key, name = NULL;
	CFDictionaryRef dict;

	if (store == NULL)
		return (NULL);
	key = SCDynamicStoreKeyCreateNetworkGlobalEntity(NULL,
	    kSCDynamicStoreDomainState, CFSTR("HostNames"));
	if (key == NULL)
		return (NULL);
	dict = (CFDictionaryRef)SCDynamicStoreCopyValue(store, key);
	CFRelease(key);
	if (dict == NULL)
		return (NULL);
	if (CFGetTypeID(dict) == CFDictionaryGetTypeID()) {
		CFStringRef v = CFDictionaryGetValue(dict,
		    kSCPropNetLocalHostName);
		if (v != NULL && CFGetTypeID(v) == CFStringGetTypeID() &&
		    CFStringGetLength(v) > 0)
			name = CFStringCreateCopy(NULL, v);
	}
	CFRelease(dict);
	return (name);
}

/* Read the current SCPrefs ComputerName from /System/System, so we only
 * persist (and trigger the downstream republish chain) when the resolved
 * name actually differs. NULL if absent. Caller releases. */
static CFStringRef
read_prefs_computer_name(void)
{
	SCPreferencesRef prefs;
	CFStringRef path, name = NULL;
	CFDictionaryRef sysdict;

	prefs = SCPreferencesCreate(NULL,
	    CFSTR("com.apple.hostnamed.observer.read"), NULL);
	if (prefs == NULL)
		return (NULL);
	path = CFStringCreateWithCString(NULL, "/System/System",
	    kCFStringEncodingUTF8);
	if (path != NULL) {
		sysdict = SCPreferencesPathGetValue(prefs, path);
		CFRelease(path);
		if (sysdict != NULL &&
		    CFGetTypeID(sysdict) == CFDictionaryGetTypeID()) {
			CFStringRef v = CFDictionaryGetValue(sysdict,
			    CFSTR("ComputerName"));
			if (v != NULL && CFGetTypeID(v) == CFStringGetTypeID())
				name = CFStringCreateCopy(NULL, v);
		}
	}
	CFRelease(prefs);
	return (name);
}

/* Persist `name` into SCPreferences /System/System ComputerName + commit.
 * Mirrors hostnameprefset.c. Returns 0 on success. The commit fires
 * prefs_monitor's SCPrefs callback, which republishes the Setup: keys the
 * set-hostname.c engine reads. */
static int
persist_computer_name(CFStringRef name)
{
	SCPreferencesRef prefs;
	CFStringRef path, key;
	CFMutableDictionaryRef dict;
	int rc = -1;

	prefs = SCPreferencesCreate(NULL,
	    CFSTR("com.apple.hostnamed.observer"), NULL);
	if (prefs == NULL) {
		xlog("hostname_observer: SCPreferencesCreate failed: %s",
		    SCErrorString(SCError()));
		return (-1);
	}
	path = CFStringCreateWithCString(NULL, "/System/System",
	    kCFStringEncodingUTF8);
	key = CFStringCreateWithCString(NULL, "ComputerName",
	    kCFStringEncodingUTF8);
	dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (path != NULL && key != NULL && dict != NULL) {
		CFDictionarySetValue(dict, key, name);
		if (!SCPreferencesPathSetValue(prefs, path, dict))
			xlog("hostname_observer: PathSetValue failed: %s",
			    SCErrorString(SCError()));
		else if (!SCPreferencesCommitChanges(prefs))
			xlog("hostname_observer: CommitChanges failed: %s",
			    SCErrorString(SCError()));
		else
			rc = 0;
	}
	if (dict != NULL) CFRelease(dict);
	if (key != NULL) CFRelease(key);
	if (path != NULL) CFRelease(path);
	CFRelease(prefs);
	return (rc);
}

/* Core action: read the resolved name from State:, and if it differs from
 * the current SCPrefs ComputerName, persist it. Shared by the initial
 * startup check and the SCDS change callback. */
static void
reconcile(SCDynamicStoreRef store)
{
	CFStringRef resolved, current;
	char rbuf[256];

	resolved = read_state_local_hostname(store);
	if (resolved == NULL)
		return;	/* no conflict-resolved name published (yet) */

	if (!CFStringGetCString(resolved, rbuf, sizeof(rbuf),
	    kCFStringEncodingUTF8))
		rbuf[0] = '\0';

	current = read_prefs_computer_name();
	if (current != NULL && CFEqual(current, resolved)) {
		/* Already persisted — nothing to do (loop guard). */
		CFRelease(current);
		CFRelease(resolved);
		return;
	}
	if (current != NULL)
		CFRelease(current);

	if (persist_computer_name(resolved) == 0) {
		/*
		 * Persisting to SCPrefs is the durable record, but set-hostname.c
		 * only re-reads ComputerName when configd notifies it of the
		 * Setup:/System change — and in our configd Setup: notifications
		 * are not reliably delivered (the engine wakes on State:/IPv4
		 * churn instead), so the kernel hostname can lag the rename until
		 * the next churn. Apply the resolved name to the kernel directly
		 * here so it takes effect promptly. set-hostname.c converges on the
		 * same value on its next run (ComputerName now equals rbuf), so
		 * this does not fight the engine.
		 */
		if (rbuf[0] != '\0' &&
		    sethostname(rbuf, (int)strlen(rbuf)) != 0)
			xlog("hostname_observer: sethostname('%s') failed", rbuf);
		xlog("hostname_observer: persisted + applied conflict-resolved "
		    "hostname '%s' (SCPrefs ComputerName + kernel)", rbuf);
	}
	CFRelease(resolved);
}

/* SCDS callback: State:/Network/HostNames changed (mDNSResponder published
 * a conflict-resolved name). Reconcile into SCPrefs. */
static void
observer_cb(SCDynamicStoreRef store, CFArrayRef changedKeys, void *info)
{
	(void)changedKeys;
	(void)info;
	reconcile(store);
}

/* Entry: open SCDS, subscribe to State:/Network/HostNames on the supplied
 * dispatch queue, do an initial reconcile (covers a conflict already
 * published before we started). Returns 0 on success; non-zero on a
 * failure that prevents observation. The caller (hostnamed's main) keeps
 * `queue` alive for the daemon's lifetime. */
int
hostname_observer_start(dispatch_queue_t queue)
{
	CFMutableArrayRef keys;
	CFStringRef key;
	SCDynamicStoreContext sctx = {0, &g_state, NULL, NULL, NULL};

	memset(&g_state, 0, sizeof(g_state));
	g_state.queue = queue;
	g_state.store = SCDynamicStoreCreate(NULL,
	    CFSTR("com.apple.hostnamed.observer"), observer_cb, &sctx);
	if (g_state.store == NULL) {
		xlog("hostname_observer: SCDynamicStoreCreate failed");
		return (-1);
	}

	key = SCDynamicStoreKeyCreateNetworkGlobalEntity(NULL,
	    kSCDynamicStoreDomainState, CFSTR("HostNames"));
	if (key == NULL) {
		xlog("hostname_observer: key create failed");
		return (-1);
	}
	keys = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	if (keys == NULL) {
		CFRelease(key);
		return (-1);
	}
	CFArrayAppendValue(keys, key);
	CFRelease(key);

	if (!SCDynamicStoreSetNotificationKeys(g_state.store, keys, NULL)) {
		xlog("hostname_observer: SetNotificationKeys failed");
		CFRelease(keys);
		return (-1);
	}
	CFRelease(keys);

	if (!SCDynamicStoreSetDispatchQueue(g_state.store, queue)) {
		xlog("hostname_observer: SetDispatchQueue failed");
		return (-1);
	}

	/* Initial reconcile: if mDNSResponder already published a resolved
	 * name before this observer subscribed, persist it now. */
	reconcile(g_state.store);

	xlog("hostname_observer: watching State:/Network/HostNames for "
	    "Bonjour conflict-rename feedback (#156)");
	return (0);
}
