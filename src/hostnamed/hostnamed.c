/*
 * hostnamed — Apple-shape persistent hostname decision daemon.
 *
 * iter 3c: structurally tiny. Two components do all the work, both
 * scheduled on a shared libdispatch serial queue:
 *
 *   prefs_monitor (src/hostnamed/prefs_monitor.c, clean-room ~100 LOC)
 *     Bridges /Library/Preferences/SystemConfiguration/preferences.plist
 *     to the SCDynamicStore Setup: keys that the decision engine reads.
 *     PreferencesMonitor-equivalent: the configd plugin we don't have.
 *
 *   load_hostname (src/hostnamed/vendored/set-hostname.c, APSL 2.0
 *     vendored ~540 LOC from configd/Plugins/IPMonitor/set-hostname.c)
 *     The Apple-canonical decision engine. Subscribes to SCDS for
 *     ComputerName / HostNames / DHCP and to the network-change
 *     notify(3) token; on every event, picks a name from the chain
 *     SCPrefs → primary-service DHCP Option 12 → reverse PTR → mDNS
 *     LocalHostName → freebsd_synthesize_hostname (substitutes
 *     Apple's "localhost" fallback per hostnamed plan §Q6);
 *     sethostname() + notify_post("com.apple.system.hostname").
 *
 * Plus a small freebsd-shim layer (src/hostnamed/freebsd-shim/)
 * implementing the IPMonitor-internal extern declarations and the
 * SCPrivate / SCValidation SPI subset set-hostname.c uses.
 *
 * Predecessor iterations (1, 2, 3a, 3b) carried a clean-room
 * decision chain in this file (try_override / try_scprefs / try_dhcp
 * / try_mdns / synthesize). That code is gone; the synthesis machinery
 * was extracted into the freebsd-shim where freebsd_synthesize_hostname
 * wraps it, and the per-tier readers are replaced by Apple's vendored
 * implementation.
 *
 * Plan: https://pkgdemon.github.io/freebsd-hostnamed-plan.html
 */

#include "freebsd-shim/ip_plugin.h"

#include <CoreFoundation/CoreFoundation.h>

#include <dispatch/dispatch.h>

#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The set-hostname.c entry point — extern from src/hostnamed/vendored/. */
extern void	load_hostname(dispatch_queue_t S_queue);

/* prefs_monitor entry — extern from src/hostnamed/prefs_monitor.c. */
extern int	prefs_monitor_start(dispatch_queue_t queue);

/* Shared logger used by every component in the daemon. Format-string
 * variadic via vfprintf; stderr → /var/log/hostnamed.stderr via the
 * launchd plist's StandardErrorPath. UTC timestamp per line. */
void
xlog(const char *fmt, ...)
{
	struct timespec ts;
	struct tm tm;
	char tbuf[32];
	va_list ap;

	(void)clock_gettime(CLOCK_REALTIME, &ts);
	(void)gmtime_r(&ts.tv_sec, &tm);
	(void)strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", &tm);
	(void)fprintf(stderr, "hostnamed %s ", tbuf);
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

static void
sigterm_handler(void *ctx)
{
	(void)ctx;
	xlog("SIGTERM/INT — exiting");
	exit(0);
}

int
main(int argc, char **argv)
{
	dispatch_queue_t queue;
	dispatch_source_t sig_term_src, sig_int_src;
	sigset_t mask;

	(void)argc;
	(void)argv;

	xlog("starting (iter 3c: vendored Apple set-hostname.c decision engine)");

	/* Boot-time sethostname so the kernel has a usable hostname
	 * BEFORE getty fires its banner and BEFORE set-hostname.c's
	 * decision engine starts. set-hostname.c's update_hostname
	 * may stay async in PTR cancel/restart loops while ipconfigd
	 * is renewing DHCP; without this boot-time set, the kernel
	 * stays at FreeBSD's default "localhost" until the engine
	 * settles. The synthesized value is also what
	 * freebsd_synthesize_hostname returns as the engine's
	 * "localhost" carry fallback. */
	{
		CFStringRef synth = freebsd_synthesize_hostname();
		char buf[256];

		if (synth != NULL && CFStringGetCString(synth, buf,
		    sizeof(buf), kCFStringEncodingUTF8)) {
			if (sethostname(buf, (int)strlen(buf)) == 0) {
				xlog("boot-time sethostname('%s') OK", buf);
			} else {
				xlog("boot-time sethostname('%s') FAILED",
				    buf);
			}
		}
		if (synth != NULL) CFRelease(synth);
	}

	queue = dispatch_queue_create("com.apple.hostnamed.events", NULL);
	if (queue == NULL) {
		xlog("HOSTNAMED-FAIL: dispatch_queue_create");
		return (1);
	}

	/* prefs_monitor publishes Setup:/System + Setup:/Network/HostNames
	 * FIRST (synthesized fallback if SCPrefs ComputerName is absent),
	 * so by the time the vendored engine wakes up those keys are
	 * populated. */
	if (prefs_monitor_start(queue) != 0) {
		xlog("HOSTNAMED-FAIL: prefs_monitor_start");
		return (1);
	}

	/* The decision engine. Subscribes + idles; every event reactively
	 * re-runs the chain and sethostname()s the result. */
	xlog("pre-load_hostname");
	load_hostname(queue);
	xlog("post-load_hostname");
	xlog("HOSTNAMED-OK: load_hostname scheduled (Apple-shape engine "
	    "subscribed to SCDS + notify_register_dispatch)");

	/* Block + dispatch-source SIGTERM/INT for clean shutdown. Pattern
	 * from src/Libnotify/notifyd/notifyd.c:1480-1503. */
	(void)sigemptyset(&mask);
	(void)sigaddset(&mask, SIGTERM);
	(void)sigaddset(&mask, SIGINT);
	(void)sigprocmask(SIG_BLOCK, &mask, NULL);
	xlog("post-sigprocmask");

	sig_term_src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL,
	    (uintptr_t)SIGTERM, 0, queue);
	dispatch_source_set_event_handler_f(sig_term_src, sigterm_handler);
	dispatch_activate(sig_term_src);
	xlog("post-SIGTERM-dispatch-source");

	sig_int_src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL,
	    (uintptr_t)SIGINT, 0, queue);
	dispatch_source_set_event_handler_f(sig_int_src, sigterm_handler);
	dispatch_activate(sig_int_src);
	xlog("post-SIGINT-dispatch-source");

	xlog("event loop entered");
	dispatch_main();
	/* NOTREACHED */
}
