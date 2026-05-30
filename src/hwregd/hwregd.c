/*
 * hwregd.c — freebsd-launchd-mach hardware registry daemon.
 *
 * Phase 0 / iter 3b-ii: Mach pub/sub bus. Reads kernel device
 * events from /dev/devctl and parses each by class:
 *   `?` nomatch -> match against the merged /boot/kernel +
 *                  /boot/modules linker.hints; for a genuine
 *                  hot-plug event, kldload(2) the driver that claims
 *                  it (closes the FreeBSD-devd autoload gap, commit
 *                  88694f0).
 *   `+` attach  -> a device and its driver came up.
 *   `-` detach  -> a device went away.
 *   `!` notify  -> a subsystem state change (link up/down, ACPI
 *                  lid, CAM errors, ...).
 * Parsed events are logged and published over a Mach-RPC pub/sub
 * bus: clients look up "org.freebsd.hwregd", send a SUBSCRIBE, and
 * receive EVENT messages. launchd HardwareMatch will be one client.
 *
 * Backlog vs. live: at startup the kernel hands hwregd a backlog of
 * `?` events buffered from early boot. kldload'ing those mid-boot
 * used to wedge the CI boot test — a matched driver's attach() ran
 * under kernel locks the boot probe still held (CI offender: ichsmb
 * for ICH SMBus). The previous workaround was a 60-second wall-clock
 * settle window after which the queued matches were drained.
 *
 * The wall-clock window is gone. FreeBSD has a real signal for this:
 * the DEV_FREEZE / DEV_THAW ioctls on /dev/devctl (sys/kern/subr_bus.c
 * 6003-6016), wrapped by libdevctl as devctl_freeze() / devctl_thaw().
 * /etc/rc.d/devmatch on stock FreeBSD wraps its kldload loop in
 * `devctl freeze ... devctl thaw` for the same reason: while frozen,
 * the kernel batches newly-loaded drivers' attach() into a deferred
 * list (device_do_deferred_actions, subr_bus.c:5699) instead of
 * running them recursively under the in-flight probe locks. The thaw
 * then drains them in one pass after the kldload batch is complete —
 * no more cold-boot probe contention, no wall-clock guess.
 *
 * Boot flow now: drain whatever devctl backlog is already queued
 * (collecting unique module names to load), freeze, kldload each,
 * thaw — emits HWREG-AUTOLOAD-OK as before. After the initial drain,
 * hwregd enters live mode and kldload's each hot-plug nomatch inline
 * (the cold-boot probe is over by then, no freeze/thaw needed).
 *
 * The hints-matching machinery — read_linker_hints / search_hints
 * and the pnpinfo helpers (getint, getstr, pnpval_as_int,
 * pnpval_as_str, quoted_strcpy) — is ported near-verbatim from
 * FreeBSD's sbin/devmatch/devmatch.c (BSD-2-Clause, Netflix 2017);
 * fatal err()/errx()/warnx() become log-and-return so the daemon
 * survives a missing or corrupt hints file, and a match is logged
 * rather than printed. devmatch's libdevinfo "attached once" filter
 * is dropped — every `?` event is a live "no driver" report.
 *
 * Eventual scope: Phase 0 also publishes +attach/-detach/!notify
 * events over a simple Mach-RPC pub/sub; Phase 1 builds the full
 * IORegistry tree + IOKit-shape query API on top.
 *
 * Plan: https://pkgdemon.github.io/freebsd-hardware-registry-iokit-plan.html
 *
 * devctl event line format (sys/kern/kern_devctl.c):
 *   +em0 at bus=0x0 slot=0x19 ... on pci0       attach
 *   -em0 at ... on pci0                          detach
 *   ? at <location + pnpinfo> on <bus><unit>     nomatch
 *   !system=IFNET subsystem=em0 type=LINK_UP     notify
 * First byte is the event class; the rest is space-separated
 * key=value pairs.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/linker.h>
#include <sys/module.h>
#include <sys/pciio.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

#include <ctype.h>
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include "hwreg_mig_types.h"	/* HWREG_MSG_*, struct hwreg_event_msg */
#include "hwreg_registry.h"
#include "hwregServer.h"		/* MIG: hwreg_server() demux + routine protos */
#include "nv.h"			/* libxpc nvlist — property-bag (de)serialise */

#define DEVCTL_PATH		"/dev/devctl"
#define READ_BUF_SIZE		(64 * 1024)
#define EVENT_LINE_MAX		1024

/* Mach service name hwregd checks in with launchd for its pub/sub bus. */
#define HWREGD_SERVICE_NAME	"org.freebsd.hwregd"

/*
 * Mach pub/sub wire protocol (iter 3b-ii). A client looks the service
 * up, sends a bare-header SUBSCRIBE (its own notify port travels as
 * the message's remote port), and then receives EVENT messages.
 *
 * HWREG_MSG_{SUBSCRIBE,EVENT} + struct hwreg_event_msg live in
 * hwreg_mig_types.h so the IOKit facade (src/libIOKit/IOKitNotify.c)
 * speaks the same protocol when demuxing watch events.
 */

/*
 * Mach-RPC query API (iter 2a) — MIG subsystem 30000, demuxed by
 * hwreg_server() in the same receive loop. HWREG_RPC_BUFSZ sizes the
 * request + reply buffers (the largest message is hwreg_get_children's
 * id-array reply). HWREG_RPC_MAX_CHILDREN must match the array bound
 * `array[*:128]` in hwreg.defs.
 */
#define HWREG_RPC_BUFSZ		4096
#define HWREG_RPC_MAX_CHILDREN	128
#define HWREG_RPC_BLOBSZ	2048	/* hwreg_blob_t bound — matches hwreg.defs */

#define HWREGD_MAX_SUBSCRIBERS	32
#define HWREGD_MAX_WATCHERS	32

static volatile sig_atomic_t got_term = 0;

/*
 * Subscriber registry. The Mach thread appends on SUBSCRIBE; the
 * devctl (main) thread reads it to publish. Guarded by the mutex.
 */
static mach_port_t	subscribers[HWREGD_MAX_SUBSCRIBERS];
static int		n_subscribers;
static pthread_mutex_t	subscribers_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Watcher registry (iter 3). Like the subscriber registry, but each
 * entry carries a criteria filter + an event mask: the Mach thread
 * adds/removes on hwreg_watch/_unwatch, the devctl thread reads it in
 * notify_watchers(). An empty criterion string means "match any".
 */
struct hwreg_watcher {
	uint64_t	id;		/* watcher id; 0 = free slot */
	mach_port_t	port;		/* client notify port (send right) */
	uint32_t	mask;		/* HWREG_EVT_* bits */
	char		want_name[32];
	char		want_class[32];
	char		want_driver[32];
};
static struct hwreg_watcher watchers[HWREGD_MAX_WATCHERS];
static int		n_watchers;
static uint64_t		next_watcher_id = 1;
static pthread_mutex_t	watchers_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * devmatch carried these as command-line flags. hwregd only runs
 * the match-and-load path, so they are compile-time false — keeping
 * them lets search_hints() keep devmatch's branch structure intact
 * (the dead branches fold away) without disturbing the pointer-
 * advancing getint()/getstr() logic interleaved with the dump path.
 * dump_flag gates devmatch's table-dump; unbound_flag gates its
 * skip of kernel-built-in (lastmod == "kernel") pnp tables.
 */
static const bool dump_flag = false;
static const bool unbound_flag = false;

/* linker.hints, loaded once at startup. NULL if unavailable. */
static void *hints;
static void *hints_end;

/*
 * false until the initial devctl backlog is drained. hwregd kldloads
 * inline only in live mode; matches during the backlog drain are
 * logged + queued in deferred_mods[] and then drained in one
 * devctl_freeze + kldload + devctl_thaw pass at the flip — see
 * act_on_match() and drain_deferred_loads().
 */
static bool live_mode = false;

/*
 * Backlog-of-matches queue. During the initial backlog drain each
 * module name a nomatch resolves to is appended here (deduped). At the
 * flip to live mode drain_deferred_loads() walks the queue and kldload(2)s
 * each entry. 64 slots × 32 bytes = 2 KiB — comfortably more than the
 * handful of PCI/USB device classes typical hardware needs a driver
 * for at boot.
 */
#define HWREGD_DEFERRED_MAX	64
#define HWREGD_DEFERRED_MODLEN	32
static char	deferred_mods[HWREGD_DEFERRED_MAX][HWREGD_DEFERRED_MODLEN];
static int	n_deferred;

static void
on_signal(int sig)
{
	got_term = sig;
}

static void
xlog(const char *fmt, ...)
{
	struct timespec ts;
	char tbuf[32];

	(void)clock_gettime(CLOCK_REALTIME, &ts);
	{
		struct tm tm;
		(void)gmtime_r(&ts.tv_sec, &tm);
		(void)strftime(tbuf, sizeof(tbuf),
		    "%Y-%m-%dT%H:%M:%SZ", &tm);
	}
	(void)fprintf(stderr, "hwregd %s ", tbuf);
	{
		va_list ap;
		va_start(ap, fmt);
		(void)vfprintf(stderr, fmt, ap);
		va_end(ap);
	}
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
}

/*
 * Send one event message to a subscriber's port. MACH_SEND_TIMEOUT so
 * a dead or backed-up subscriber can't block hwregd. The registry
 * holds send rights, so the disposition is COPY_SEND. Returns the
 * mach_msg result so the caller can prune a subscriber that has gone
 * away (MACH_SEND_INVALID_DEST).
 */
static mach_msg_return_t
send_event_to(mach_port_t dst, char kind, const char *text)
{
	struct hwreg_event_msg m;
	mach_msg_return_t mr;

	memset(&m, 0, sizeof(m));
	m.hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	m.hdr.msgh_size = sizeof(m);
	m.hdr.msgh_remote_port = dst;
	m.hdr.msgh_local_port = MACH_PORT_NULL;
	m.hdr.msgh_id = HWREG_MSG_EVENT;
	m.kind = kind;
	strlcpy(m.text, text, sizeof(m.text));

	mr = mach_msg(&m.hdr, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
	    sizeof(m), 0, MACH_PORT_NULL, 200, MACH_PORT_NULL);
	if (mr != MACH_MSG_SUCCESS)
		xlog("Mach: send to subscriber 0x%x failed: 0x%x",
		    (unsigned)dst, (unsigned)mr);
	return mr;
}

/*
 * Drop a subscriber whose notify port has died. Called when a send
 * fails MACH_SEND_INVALID_DEST — the client exited and its port is now
 * a dead name. Without this every later publish keeps re-sending (and
 * re-failing) to it and the 32-slot table never frees the entry.
 *
 * The port is matched by value, not index: publish_event() snapshots
 * the registry and sends outside the lock, so the live array may have
 * shifted by the time we get here. The dead name is then deallocated
 * so it doesn't leak hwregd's IPC space. A live-but-slow subscriber
 * fails MACH_SEND_TIMED_OUT (not INVALID_DEST) and is never dropped.
 */
static void
drop_subscriber(mach_port_t dead)
{
	int i, total = -1;

	pthread_mutex_lock(&subscribers_lock);
	for (i = 0; i < n_subscribers; i++) {
		if (subscribers[i] == dead) {
			/* Compact: move the last entry into the gap. */
			subscribers[i] = subscribers[--n_subscribers];
			total = n_subscribers;
			break;
		}
	}
	pthread_mutex_unlock(&subscribers_lock);

	if (total >= 0) {
		xlog("Mach: dropped dead subscriber 0x%x (total=%d)",
		    (unsigned)dead, total);
		(void)mach_port_deallocate(mach_task_self(), dead);
	}
}

/*
 * Publish a parsed device event to every subscriber. The registry is
 * snapshotted under the lock and the sends happen outside it, so a
 * slow send never blocks a new subscriber from being registered. A
 * subscriber whose port has died is pruned via drop_subscriber().
 */
static void
publish_event(char kind, const char *text)
{
	mach_port_t snap[HWREGD_MAX_SUBSCRIBERS];
	int i, n;

	pthread_mutex_lock(&subscribers_lock);
	n = n_subscribers;
	memcpy(snap, subscribers, (size_t)n * sizeof(snap[0]));
	pthread_mutex_unlock(&subscribers_lock);

	for (i = 0; i < n; i++) {
		if (send_event_to(snap[i], kind, text) ==
		    MACH_SEND_INVALID_DEST)
			drop_subscriber(snap[i]);
	}
}

/*
 * Drop the watcher holding `port` — called when an event push fails
 * MACH_SEND_INVALID_DEST (the client exited). Mirrors drop_subscriber.
 */
static void
watcher_drop(mach_port_t port)
{
	int i, found = 0;

	pthread_mutex_lock(&watchers_lock);
	for (i = 0; i < n_watchers; i++) {
		if (watchers[i].port == port) {
			xlog("Mach: dropped dead watcher %ju (port 0x%x)",
			    (uintmax_t)watchers[i].id, (unsigned)port);
			watchers[i] = watchers[--n_watchers];
			found = 1;
			break;
		}
	}
	pthread_mutex_unlock(&watchers_lock);
	if (found)
		(void)mach_port_deallocate(mach_task_self(), port);
}

/*
 * Fan a device arrival/departure out to the watchers whose criteria
 * and event mask match it. Called from the devctl thread after the
 * registry has absorbed the event, so the node's class and driver
 * are available to match against. An empty criterion matches any.
 */
static void
notify_watchers(char kind, const char *devname)
{
	uint32_t evt = (kind == '+') ? HWREG_EVT_ARRIVED : HWREG_EVT_DEPARTED;
	struct hwreg_watcher snap[HWREGD_MAX_WATCHERS];
	struct hw_node n;
	char text[EVENT_LINE_MAX];
	int i, ns;

	if (!hwreg_copy_by_name(devname, &n))
		return;			/* not in the registry — nothing to match */

	pthread_mutex_lock(&watchers_lock);
	ns = n_watchers;
	memcpy(snap, watchers, (size_t)ns * sizeof(snap[0]));
	pthread_mutex_unlock(&watchers_lock);

	(void)snprintf(text, sizeof(text), "%s id=%ju name=%s class=%s",
	    (kind == '+') ? "arrived" : "departed",
	    (uintmax_t)n.id, n.name, n.classname);

	for (i = 0; i < ns; i++) {
		if ((snap[i].mask & evt) == 0)
			continue;
		if (snap[i].want_name[0] != '\0' &&
		    strcmp(snap[i].want_name, n.name) != 0)
			continue;
		if (snap[i].want_class[0] != '\0' &&
		    strcmp(snap[i].want_class, n.classname) != 0)
			continue;
		if (snap[i].want_driver[0] != '\0' &&
		    strcmp(snap[i].want_driver, n.driver) != 0)
			continue;
		if (send_event_to(snap[i].port, kind, text) ==
		    MACH_SEND_INVALID_DEST)
			watcher_drop(snap[i].port);
	}
}

/*
 * Append `mod` to the deferred backlog queue, deduping by name. Used
 * during the settle window so we don't kldload(2) mid-boot; the queue
 * is drained at the flip to live mode by drain_deferred_loads().
 */
static void
defer_match(const char *mod)
{
	int i;

	for (i = 0; i < n_deferred; i++) {
		if (strcmp(deferred_mods[i], mod) == 0)
			return;
	}
	if (n_deferred >= HWREGD_DEFERRED_MAX) {
		xlog("deferred backlog full (%d slots) — dropping '%s'",
		    HWREGD_DEFERRED_MAX, mod);
		return;
	}
	strlcpy(deferred_mods[n_deferred], mod,
	    sizeof(deferred_mods[n_deferred]));
	n_deferred++;
}

/*
 * Act on a module that linker.hints says claims a nomatched device.
 * "kernel" is the pseudo-module for built-in drivers — nothing to
 * load. In live mode (hot-plug) the module is kldload(2)ed inline;
 * during the boot backlog the module name is queued for the drain
 * at live-mode flip — kldloading mid-boot wedges the boot (a driver
 * attach runs under kernel locks; see the file header). EEXIST
 * (already loaded) is a normal, expected outcome.
 */
static void
act_on_match(const char *mod)
{
	if (mod == NULL || mod[0] == '\0' || strcmp(mod, "kernel") == 0)
		return;

	if (!live_mode) {
		xlog("match: module '%s' claims this device "
		    "(boot backlog — queued for autoload)", mod);
		defer_match(mod);
		return;
	}

	if (kldload(mod) < 0) {
		if (errno == EEXIST)
			xlog("kldload(%s): already loaded", mod);
		else
			xlog("kldload(%s) failed: %s", mod, strerror(errno));
	} else {
		xlog("kldload(%s): loaded", mod);
	}
}

/*
 * Drain the boot-backlog deferred-match queue: kldload(2) each unique
 * module a backlog nomatch resolved to. Called exactly once at the
 * flip to live mode.
 *
 * Wrapped in devctl_freeze() / devctl_thaw() so the kernel batches
 * the new drivers' attach() into device_do_deferred_actions
 * (sys/kern/subr_bus.c:5699) instead of running them recursively
 * under the in-flight cold-boot probe locks. /etc/rc.d/devmatch
 * uses the same pattern on stock FreeBSD. If freeze/thaw fail
 * (typically ENXIO when /dev/devctl isn't writable, or if the kernel
 * is too old to support DEV_FREEZE — present since FreeBSD 11), fall
 * back to bare kldload(2): we lose the cascade-batching protection
 * but the daemon still functions and the failure is logged for
 * diagnosis.
 *
 * Emits HWREG-AUTOLOAD-OK unconditionally (even when the queue is
 * empty — common in QEMU/SLIRP where every CI device has a built-in
 * driver) so CI can prove the drain ran.
 */
static void
drain_deferred_loads(void)
{
	int i, loaded = 0, existing = 0, failed = 0;
	bool frozen;

	frozen = (devctl_freeze() == 0);
	if (!frozen) {
		xlog("devctl_freeze failed: %s — proceeding without "
		    "cascade-batching protection", strerror(errno));
	}

	xlog("draining %d deferred boot-backlog match(es)%s",
	    n_deferred, frozen ? " under devctl freeze" : "");
	for (i = 0; i < n_deferred; i++) {
		if (kldload(deferred_mods[i]) < 0) {
			if (errno == EEXIST) {
				xlog("kldload(%s): already loaded",
				    deferred_mods[i]);
				existing++;
			} else {
				xlog("kldload(%s) failed: %s",
				    deferred_mods[i], strerror(errno));
				failed++;
			}
		} else {
			xlog("kldload(%s): loaded", deferred_mods[i]);
			loaded++;
		}
	}
	n_deferred = 0;

	if (frozen && devctl_thaw() != 0) {
		xlog("devctl_thaw failed: %s — kernel may stay frozen "
		    "until next freeze/thaw cycle", strerror(errno));
	}

	xlog("HWREG-AUTOLOAD-OK: drained queue "
	    "(loaded=%d already=%d failed=%d)",
	    loaded, existing, failed);
}

/* --- linker.hints reader (ported from devmatch.c) ----------------- */

static void *
read_hints(const char *fn, size_t *len)
{
	void *h;
	int fd;
	struct stat sb;
	ssize_t rd;

	fd = open(fn, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	if (fstat(fd, &sb) != 0) {
		xlog("fstat(%s) failed: %s", fn, strerror(errno));
		(void)close(fd);
		return NULL;
	}
	h = malloc((size_t)sb.st_size);
	if (h == NULL) {
		xlog("out of memory reading %s (%ju bytes)",
		    fn, (uintmax_t)sb.st_size);
		(void)close(fd);
		return NULL;
	}
	rd = read(fd, h, (size_t)sb.st_size);
	(void)close(fd);
	if (rd != (ssize_t)sb.st_size) {
		xlog("short read on %s", fn);
		free(h);
		return NULL;
	}
	*len = (size_t)sb.st_size;
	return h;
}

/*
 * Load and merge linker.hints from every directory in
 * kern.module_path. Sets the hints / hints_end globals; leaves them
 * NULL (and logs) if no usable hints file is found.
 */
static void
read_linker_hints(void)
{
	char fn[MAXPATHLEN];
	char *modpath, *p, *q;
	void *all_hints = NULL;
	size_t buflen, len = 0, all_len = 0;

	if (sysctlbyname("kern.module_path", NULL, &buflen, NULL, 0) < 0) {
		xlog("sysctl kern.module_path failed: %s", strerror(errno));
		return;
	}
	modpath = malloc(buflen);
	if (modpath == NULL) {
		xlog("out of memory for module path");
		return;
	}
	if (sysctlbyname("kern.module_path", modpath, &buflen, NULL, 0) < 0) {
		xlog("sysctl kern.module_path failed: %s", strerror(errno));
		free(modpath);
		return;
	}

	p = modpath;
	while ((q = strsep(&p, ";")) != NULL) {
		void *h;

		if (*q == '\0')
			continue;
		(void)snprintf(fn, sizeof(fn), "%s/linker.hints", q);
		h = read_hints(fn, &len);
		if (h == NULL)
			continue;
		if (len < sizeof(int) ||
		    *(int *)(intptr_t)h != LINKER_HINTS_VERSION) {
			xlog("ignoring %s (bad version/too short)", fn);
			free(h);
			continue;
		}
		if (all_hints == NULL) {
			all_hints = h;
			all_len = len;
		} else {
			void *merged = realloc(all_hints,
			    all_len + len - sizeof(int));
			if (merged == NULL) {
				free(h);
				continue;
			}
			all_hints = merged;
			memcpy((char *)all_hints + all_len,
			    (char *)h + sizeof(int), len - sizeof(int));
			all_len += len - sizeof(int);
			free(h);
		}
		xlog("loaded %s", fn);
	}
	free(modpath);

	if (all_hints == NULL) {
		xlog("no usable linker.hints found — nomatch autoload disabled");
		return;
	}
	hints = all_hints;
	hints_end = (void *)((intptr_t)hints + (intptr_t)all_len);
}

/* --- hints record + pnpinfo helpers (verbatim from devmatch.c) ---- */

static int
getint(void **ptr)
{
	int *p = *ptr;
	int rv;

	p = (int *)roundup2((intptr_t)p, sizeof(int));
	rv = *p++;
	*ptr = p;
	return rv;
}

static void
getstr(void **ptr, char *val)
{
	int *p = *ptr;
	char *c = (char *)p;
	int len = *(uint8_t *)c;

	memcpy(val, c + 1, len);
	val[len] = 0;
	c += len + 1;
	*ptr = (void *)c;
}

static int
pnpval_as_int(const char *val, const char *pnpinfo)
{
	int rv;
	char key[256];
	char *cp;

	if (pnpinfo == NULL)
		return -1;

	cp = strchr(val, ';');
	key[0] = ' ';
	if (cp == NULL)
		strlcpy(key + 1, val, sizeof(key) - 1);
	else {
		memcpy(key + 1, val, cp - val);
		key[cp - val + 1] = '\0';
	}
	strlcat(key, "=", sizeof(key));
	if (strncmp(key + 1, pnpinfo, strlen(key + 1)) == 0)
		rv = strtol(pnpinfo + strlen(key + 1), NULL, 0);
	else {
		cp = strstr(pnpinfo, key);
		if (cp == NULL)
			rv = -1;
		else
			rv = strtol(cp + strlen(key), NULL, 0);
	}
	return rv;
}

static void
quoted_strcpy(char *dst, const char *src)
{
	char q = ' ';

	if (*src == '\'' || *src == '"')
		q = *src++;
	while (*src && *src != q)
		*dst++ = *src++;
	*dst++ = '\0';
}

static char *
pnpval_as_str(const char *val, const char *pnpinfo)
{
	static char retval[256];
	char key[256];
	char *cp;

	if (pnpinfo == NULL) {
		*retval = '\0';
		return retval;
	}

	cp = strchr(val, ';');
	key[0] = ' ';
	if (cp == NULL)
		strlcpy(key + 1, val, sizeof(key) - 1);
	else {
		memcpy(key + 1, val, cp - val);
		key[cp - val + 1] = '\0';
	}
	strlcat(key, "=", sizeof(key));
	if (strncmp(key + 1, pnpinfo, strlen(key + 1)) == 0)
		quoted_strcpy(retval, pnpinfo + strlen(key + 1));
	else {
		cp = strstr(pnpinfo, key);
		if (cp == NULL)
			strcpy(retval, "MISSING");
		else
			quoted_strcpy(retval, cp + strlen(key));
	}
	return retval;
}

/*
 * Walk linker.hints looking for a module whose MDT_PNP_INFO table
 * claims the device described by (bus, pnpinfo). Ported from
 * devmatch.c's search_hints(); the first matching module name is
 * copied into modout (left "" if nothing claims the device). The
 * dump_flag / unbound_flag branches are dead (compile-time false) but
 * kept so the port stays close to verbatim. Callers decide what to do
 * with the module — handle_nomatch() auto-loads it, hwreg_load_driver()
 * loads it on demand.
 */
static void
search_hints(const char *bus, const char *pnpinfo, char *modout, size_t modsz)
{
	char val1[256], val2[256];
	int ival, len, ents, i, notme, mask, bit, v;
	void *ptr, *walker;
	char *lastmod = NULL, *cp, *s;

	modout[0] = '\0';
	walker = hints;
	getint(&walker);
	while (walker < hints_end) {
		len = getint(&walker);
		ival = getint(&walker);
		ptr = walker;
		switch (ival) {
		case MDT_VERSION:
			getstr(&ptr, val1);
			ival = getint(&ptr);
			getstr(&ptr, val2);
			break;
		case MDT_MODULE:
			getstr(&ptr, val1);
			getstr(&ptr, val2);
			if (lastmod)
				free(lastmod);
			lastmod = strdup(val2);
			break;
		case MDT_PNP_INFO:
			if (!dump_flag && !unbound_flag && lastmod &&
			    strcmp(lastmod, "kernel") == 0)
				break;
			getstr(&ptr, val1);
			getstr(&ptr, val2);
			ents = getint(&ptr);
			if (strcmp(val1, "usb") == 0)
				strcpy(val1, "uhub");
			if (bus && strcmp(val1, bus) != 0)
				break;
			for (i = 0; i < ents; i++) {
				cp = val2;
				notme = 0;
				mask = -1;
				bit = -1;
				do {
					switch (*cp) {
						/* All integer fields */
					case 'I':
					case 'J':
					case 'G':
					case 'L':
					case 'M':
						ival = getint(&ptr);
						if (dump_flag)
							break;
						if (bit >= 0 &&
						    ((1 << bit) & mask) == 0)
							break;
						if (cp[2] == '#')
							break;
						v = pnpval_as_int(cp + 2, pnpinfo);
						switch (*cp) {
						case 'J':
							if (ival == -1)
								break;
							/*FALLTHROUGH*/
						case 'I':
							if (v != ival)
								notme++;
							break;
						case 'G':
							if (v < ival)
								notme++;
							break;
						case 'L':
							if (v > ival)
								notme++;
							break;
						case 'M':
							mask = ival;
							break;
						}
						break;
						/* String fields */
					case 'D':
					case 'Z':
						getstr(&ptr, val1);
						if (dump_flag)
							break;
						if (*cp == 'D')
							break;
						if (bit >= 0 &&
						    ((1 << bit) & mask) == 0)
							break;
						if (cp[2] == '#')
							break;
						s = pnpval_as_str(cp + 2, pnpinfo);
						if (strcmp(s, val1) != 0)
							notme++;
						break;
						/* Key override, must be last */
					case 'T':
						if (dump_flag)
							break;
						if (cp[strlen(cp) - 1] == ';')
							cp[strlen(cp) - 1] = '\0';
						if ((s = strstr(pnpinfo,
						    cp + 2)) == NULL)
							notme++;
						else if (s > pnpinfo &&
						    s[-1] != ' ')
							notme++;
						break;
					default:
						xlog("hints: unknown field "
						    "type %c", *cp);
						break;
					}
					bit++;
					cp = strchr(cp, ';');
					if (cp)
						cp++;
				} while (cp && *cp);
				if (!dump_flag && !notme &&
				    lastmod != NULL && modout[0] == '\0')
					strlcpy(modout, lastmod, modsz);
			}
			break;
		default:
			break;
		}
		walker = (void *)(len - sizeof(int) + (intptr_t)walker);
	}
	free(lastmod);
}

/*
 * Handle a `?` (nomatch) devctl line: "? at <pnpinfo> on <bus><unit>".
 * Ported from devmatch.c's find_nomatch() minus the exit() calls and
 * the libdevinfo "already attached once" filter. The line buffer is
 * mutated in place. (Bus search runs backwards to avoid a false
 * " on " inside a vendor string.)
 */
static void
handle_nomatch(char *nomatch)
{
	char *bus, *pnpinfo, *tmp;

	if (strlen(nomatch) < 4) {
		xlog("nomatch: line too short: '%s'", nomatch);
		return;
	}

	/* Find the bus name (with unit): the last " on " in the line. */
	tmp = nomatch + strlen(nomatch) - 4;
	while (tmp > nomatch && strncmp(tmp, " on ", 4) != 0)
		tmp--;
	if (tmp == nomatch) {
		xlog("nomatch: no bus in '%s'", nomatch);
		return;
	}
	bus = tmp + 4;
	*tmp = '\0';

	/* Strip the unit number off the bus name: "pci0" -> "pci". */
	tmp = bus + strlen(bus) - 1;
	while (tmp > bus && isdigit((unsigned char)*tmp))
		tmp--;
	*++tmp = '\0';

	/* Skip the leading '?' and the " at " separator. */
	if (*nomatch == '?')
		nomatch++;
	if (strncmp(nomatch, " at ", 4) != 0) {
		xlog("nomatch: malformed line (no ' at '): '%s'", nomatch);
		return;
	}
	pnpinfo = nomatch + 4;

	{
		char text[EVENT_LINE_MAX];

		(void)snprintf(text, sizeof(text),
		    "nomatch: bus=%s pnpinfo=%s", bus, pnpinfo);
		xlog("%s", text);
		publish_event('?', text);
	}
	if (hints == NULL)
		return;
	{
		char mod[64] = "";

		search_hints(bus, pnpinfo, mod, sizeof(mod));
		if (mod[0] != '\0')
			act_on_match(mod);
		else
			xlog("nomatch: no module in linker.hints "
			    "claims '%s'", pnpinfo);
	}
}

/*
 * Extract a space-delimited key=value field from a devctl event line.
 * Matches `key` only at a token boundary (start of line or after a
 * space) and immediately followed by '='; copies the value up to the
 * next space. Good enough for the simple system/subsystem/type
 * fields of a !notify line. Returns true if found.
 */
static bool
event_kv(const char *line, const char *key, char *out, size_t outsz)
{
	size_t keylen = strlen(key);
	const char *p = line;

	out[0] = '\0';
	while ((p = strstr(p, key)) != NULL) {
		if ((p == line || p[-1] == ' ') && p[keylen] == '=') {
			const char *v = p + keylen + 1;
			size_t i = 0;

			while (v[i] != '\0' && v[i] != ' ' && i < outsz - 1) {
				out[i] = v[i];
				i++;
			}
			out[i] = '\0';
			return true;
		}
		p += keylen;
	}
	return false;
}

/*
 * Handle a `!` (notify) devctl line:
 * "!system=X subsystem=Y type=Z ...". Kernel subsystems post these
 * for state changes — IFNET link up/down, ACPI lid/AC, CAM errors.
 */
static void
handle_notify(const char *line)
{
	char system[64], subsystem[128], type[64];
	const char *kv = line + 1;		/* skip the leading '!' */

	event_kv(kv, "system", system, sizeof(system));
	event_kv(kv, "subsystem", subsystem, sizeof(subsystem));
	event_kv(kv, "type", type, sizeof(type));
	{
		char text[EVENT_LINE_MAX];

		(void)snprintf(text, sizeof(text),
		    "notify: system=%s subsystem=%s type=%s",
		    system[0] ? system : "?",
		    subsystem[0] ? subsystem : "?",
		    type[0] ? type : "?");
		xlog("%s", text);
		publish_event('!', text);
	}
}

/*
 * Handle a `+` (attach) or `-` (detach) devctl line:
 * "+devname at <location> on <parent>". Mutates the line buffer
 * (NUL-terminates the devname and location in place).
 */
static void
handle_attach_detach(char *line, const char *kind)
{
	char *dev = line + 1;			/* past the +/- sign */
	char *at = strstr(dev, " at ");
	if (at == NULL) {
		xlog("%s: malformed '%s'", kind, line);
		return;
	}
	*at = '\0';				/* dev now NUL-terminated */

	char *loc = at + 4;			/* "<location> on <parent>" */
	const char *parent = "";
	char *on = NULL, *scan = loc;
	while ((scan = strstr(scan, " on ")) != NULL) {
		on = scan;			/* last " on " wins */
		scan += 4;
	}
	if (on != NULL) {
		*on = '\0';
		parent = on + 4;
	}
	{
		char text[EVENT_LINE_MAX];

		(void)snprintf(text, sizeof(text),
		    "%s: dev=%s parent=%s loc=[%s]", kind,
		    dev[0] ? dev : "?", parent[0] ? parent : "?", loc);
		xlog("%s", text);
		publish_event(line[0], text);
	}

	/* Fold the event into the registry tree (Phase 1). */
	if (line[0] == '+')
		hwreg_note_attach(dev, parent, loc);
	else
		hwreg_note_detach(dev);

	/* Fan it out to the matching iter-3 watchers. */
	notify_watchers(line[0], dev);
}

/* --- devctl event loop -------------------------------------------- */

static int
open_devctl(void)
{
	/* O_NONBLOCK so the initial backlog drain can read until the kernel
	 * queue is empty (read() -> EAGAIN) instead of guessing with a timer;
	 * see the backlog-drain loop in the devctl event loop. */
	int fd = open(DEVCTL_PATH, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		xlog("open(%s) failed: %s", DEVCTL_PATH, strerror(errno));
	return fd;
}

/*
 * Read whatever's pending on /dev/devctl, split into '\n'-terminated
 * lines, and dispatch each to the parser for its event class.
 * Returns bytes read+processed (>0), -1 on a fatal read error, or 0 when
 * the (O_NONBLOCK) kernel queue is empty (EAGAIN) / EOF. The backlog-drain
 * loop relies on the 0 == "queue empty" contract, so EINTR is retried
 * here rather than reported as empty.
 */
static ssize_t
drain_devctl(int fd, char *buf, size_t bufsz)
{
	ssize_t n;

	do {
		n = read(fd, buf, bufsz - 1);
	} while (n < 0 && errno == EINTR);
	if (n < 0) {
		if (errno == EAGAIN)
			return 0;	/* O_NONBLOCK: kernel queue drained */
		xlog("read(%s) failed: %s", DEVCTL_PATH, strerror(errno));
		return -1;
	}
	if (n == 0) {
		xlog("read(%s) returned EOF", DEVCTL_PATH);
		return 0;
	}
	buf[n] = '\0';

	char *p = buf;
	char *end = buf + n;
	while (p < end) {
		char *nl = memchr(p, '\n', (size_t)(end - p));
		size_t linelen = nl ? (size_t)(nl - p) : (size_t)(end - p);
		if (linelen > 0) {
			/* Mutable, NUL-terminated copy — the parsers write
			 * into the line. */
			char line[EVENT_LINE_MAX];
			size_t cl = linelen < sizeof(line) - 1 ?
			    linelen : sizeof(line) - 1;
			memcpy(line, p, cl);
			line[cl] = '\0';

			switch (line[0]) {
			case '?': handle_nomatch(line); break;
			case '+': handle_attach_detach(line, "attach"); break;
			case '-': handle_attach_detach(line, "detach"); break;
			case '!': handle_notify(line); break;
			default:  xlog("event: %s", line); break;
			}
		}
		if (!nl)
			break;
		p = nl + 1;
	}
	return n;
}

/* --- Mach-RPC query routines (MIG hwreg subsystem) ---------------- */

/*
 * Server-side implementations of the hwreg.defs routines; MIG's
 * hwreg_server() demux calls these. Each bridges the RPC to the
 * registry's internally-locked query helpers.
 */
kern_return_t
hwreg_get_root(mach_port_t server, uint64_t *root_id)
{
	(void)server;
	*root_id = hwreg_root_id();
	return KERN_SUCCESS;
}

kern_return_t
hwreg_get_children(mach_port_t server, uint64_t id,
    hwreg_id_array_t children, mach_msg_type_number_t *childrenCnt)
{
	int n = hwreg_children(id, children, HWREG_RPC_MAX_CHILDREN);

	(void)server;
	if (n < 0)
		return KERN_INVALID_ARGUMENT;	/* unknown id */
	*childrenCnt = (mach_msg_type_number_t)n;
	return KERN_SUCCESS;
}

kern_return_t
hwreg_get_node(mach_port_t server, uint64_t id, uint64_t *parent_id,
    int *node_state, hwreg_name_t name, hwreg_name_t classname,
    hwreg_name_t driver, hwreg_path_t path)
{
	struct hw_node n;

	(void)server;
	if (!hwreg_copy(id, &n))
		return KERN_INVALID_ARGUMENT;	/* unknown id */
	*parent_id = n.parent_id;
	*node_state = (int)n.state;
	strlcpy(name, n.name, sizeof(n.name));
	strlcpy(classname, n.classname, sizeof(n.classname));
	strlcpy(driver, n.driver, sizeof(n.driver));
	strlcpy(path, n.path, sizeof(n.path));
	return KERN_SUCCESS;
}

/*
 * hwreg_get_properties — node `id`'s property bag as a packed nvlist
 * dictionary. The bag is built on demand from the hw_node fields;
 * there is no stored bag until iter 4's enrichment needs one.
 */
kern_return_t
hwreg_get_properties(mach_port_t server, uint64_t id, hwreg_blob_t props,
    mach_msg_type_number_t *propsCnt)
{
	struct hw_node n;
	nvlist_t *nv;
	void *packed;
	size_t size;

	(void)server;
	if (!hwreg_copy(id, &n))
		return KERN_INVALID_ARGUMENT;	/* unknown id */

	nv = nvlist_create_dictionary(0);
	if (nv == NULL)
		return KERN_RESOURCE_SHORTAGE;
	nvlist_add_number(nv, "id", n.id);
	nvlist_add_number(nv, "parent-id", n.parent_id);
	nvlist_add_number(nv, "state", (uint64_t)n.state);
	nvlist_add_string(nv, "name", n.name);
	nvlist_add_string(nv, "class", n.classname);
	nvlist_add_string(nv, "driver", n.driver);
	nvlist_add_string(nv, "description", n.desc);
	nvlist_add_string(nv, "pnpinfo", n.pnpinfo);
	nvlist_add_string(nv, "location", n.location);
	nvlist_add_string(nv, "path", n.path);
	/* PCI identity, present only on enriched PCI nodes (iter 4b). */
	if (n.pci_vendor != 0) {
		nvlist_add_number(nv, "pci-vendor", n.pci_vendor);
		nvlist_add_number(nv, "pci-device", n.pci_device);
		nvlist_add_number(nv, "pci-subvendor", n.pci_subvendor);
		nvlist_add_number(nv, "pci-class", n.pci_class);
	}

	packed = nvlist_pack(nv, &size);
	nvlist_destroy(nv);
	if (packed == NULL)
		return KERN_RESOURCE_SHORTAGE;
	if (size > HWREG_RPC_BLOBSZ) {		/* a node bag never gets this big */
		free(packed);
		return KERN_NO_SPACE;
	}
	memcpy(props, packed, size);
	free(packed);
	*propsCnt = (mach_msg_type_number_t)size;
	return KERN_SUCCESS;
}

/* hwreg_lookup match state, threaded through hwreg_foreach(). */
struct lookup_ctx {
	const char	*want_name;
	const char	*want_class;
	const char	*want_driver;
	uint64_t	*ids;
	int		 max;
	int		 count;
};

/* hwreg_foreach() callback — record `n` if it meets every criterion. */
static void
lookup_match(const struct hw_node *n, void *arg)
{
	struct lookup_ctx *c = arg;

	if (c->want_name != NULL && strcmp(c->want_name, n->name) != 0)
		return;
	if (c->want_class != NULL && strcmp(c->want_class, n->classname) != 0)
		return;
	if (c->want_driver != NULL && strcmp(c->want_driver, n->driver) != 0)
		return;
	if (c->count < c->max)
		c->ids[c->count] = n->id;
	c->count++;
}

/*
 * hwreg_lookup — ids of the nodes matching the packed-nvlist criteria.
 * iter 2b matches the string fields name / class / driver; empty
 * criteria matches nothing.
 */
kern_return_t
hwreg_lookup(mach_port_t server, hwreg_blob_t criteria,
    mach_msg_type_number_t criteriaCnt, hwreg_id_array_t matches,
    mach_msg_type_number_t *matchesCnt)
{
	struct lookup_ctx ctx;
	nvlist_t *nv;

	(void)server;
	memset(&ctx, 0, sizeof(ctx));
	ctx.ids = matches;
	ctx.max = HWREG_RPC_MAX_CHILDREN;	/* hwreg_id_array_t bound */

	nv = nvlist_unpack(criteria, criteriaCnt);
	if (nv == NULL)
		return KERN_INVALID_ARGUMENT;	/* malformed criteria */
	if (nvlist_empty(nv)) {
		nvlist_destroy(nv);
		*matchesCnt = 0;
		return KERN_SUCCESS;
	}
	if (nvlist_exists_string(nv, "name"))
		ctx.want_name = nvlist_get_string(nv, "name");
	if (nvlist_exists_string(nv, "class"))
		ctx.want_class = nvlist_get_string(nv, "class");
	if (nvlist_exists_string(nv, "driver"))
		ctx.want_driver = nvlist_get_string(nv, "driver");

	hwreg_foreach(lookup_match, &ctx);	/* strings stay valid: nv alive */
	nvlist_destroy(nv);
	*matchesCnt = (mach_msg_type_number_t)
	    (ctx.count > ctx.max ? ctx.max : ctx.count);
	return KERN_SUCCESS;
}

/*
 * hwreg_watch — register a criteria-filtered device-event watch. The
 * notify_port arrives as a send right hwregd keeps until hwreg_unwatch
 * (or until a push to it fails and watcher_drop() prunes it).
 */
kern_return_t
hwreg_watch(mach_port_t server, hwreg_blob_t criteria,
    mach_msg_type_number_t criteriaCnt, uint32_t event_mask,
    mach_port_t notify_port, uint64_t *watcher_id)
{
	struct hwreg_watcher w;
	nvlist_t *nv;
	int total;

	(void)server;
	memset(&w, 0, sizeof(w));
	w.port = notify_port;
	w.mask = event_mask;

	/* Criteria is optional — a zero-length blob matches any device. */
	if (criteriaCnt > 0) {
		nv = nvlist_unpack(criteria, criteriaCnt);
		if (nv == NULL) {
			(void)mach_port_deallocate(mach_task_self(),
			    notify_port);
			return KERN_INVALID_ARGUMENT;	/* malformed criteria */
		}
		if (nvlist_exists_string(nv, "name"))
			strlcpy(w.want_name, nvlist_get_string(nv, "name"),
			    sizeof(w.want_name));
		if (nvlist_exists_string(nv, "class"))
			strlcpy(w.want_class, nvlist_get_string(nv, "class"),
			    sizeof(w.want_class));
		if (nvlist_exists_string(nv, "driver"))
			strlcpy(w.want_driver, nvlist_get_string(nv, "driver"),
			    sizeof(w.want_driver));
		nvlist_destroy(nv);
	}

	pthread_mutex_lock(&watchers_lock);
	if (n_watchers >= HWREGD_MAX_WATCHERS) {
		pthread_mutex_unlock(&watchers_lock);
		(void)mach_port_deallocate(mach_task_self(), notify_port);
		return KERN_RESOURCE_SHORTAGE;
	}
	w.id = next_watcher_id++;
	watchers[n_watchers++] = w;
	total = n_watchers;
	*watcher_id = w.id;
	pthread_mutex_unlock(&watchers_lock);

	xlog("Mach: watcher %ju registered (mask=0x%x, total=%d)",
	    (uintmax_t)w.id, (unsigned)event_mask, total);
	return KERN_SUCCESS;
}

/* hwreg_unwatch — cancel the watch `watcher_id`. */
kern_return_t
hwreg_unwatch(mach_port_t server, uint64_t watcher_id)
{
	mach_port_t port = MACH_PORT_NULL;
	int i;

	(void)server;
	pthread_mutex_lock(&watchers_lock);
	for (i = 0; i < n_watchers; i++) {
		if (watchers[i].id == watcher_id) {
			port = watchers[i].port;
			watchers[i] = watchers[--n_watchers];
			break;
		}
	}
	pthread_mutex_unlock(&watchers_lock);
	if (port == MACH_PORT_NULL)
		return KERN_INVALID_ARGUMENT;	/* unknown watcher_id */
	(void)mach_port_deallocate(mach_task_self(), port);
	xlog("Mach: watcher %ju cancelled", (uintmax_t)watcher_id);
	return KERN_SUCCESS;
}

/*
 * hwreg_load_driver — kldload the driver for node `id`. A node that
 * already has a driver needs nothing; otherwise its pnpinfo is run
 * through linker.hints and the claiming module is loaded. error_code
 * carries an errno (0 = ok, ENOENT = nothing in linker.hints claims
 * the device).
 */
kern_return_t
hwreg_load_driver(mach_port_t server, uint64_t id, hwreg_name_t loaded_module,
    int *error_code)
{
	struct hw_node n, parent;
	char bus[32], mod[64] = "";
	size_t k;

	(void)server;
	loaded_module[0] = '\0';
	*error_code = 0;
	if (!hwreg_copy(id, &n))
		return KERN_INVALID_ARGUMENT;		/* unknown id */

	if (n.driver[0] != '\0') {			/* already attached */
		strlcpy(loaded_module, n.driver, sizeof(hwreg_name_t));
		return KERN_SUCCESS;
	}
	if (hints == NULL) {
		*error_code = ENOENT;			/* no linker.hints */
		return KERN_SUCCESS;
	}

	/* Bus name = the parent device's name with its unit stripped. */
	bus[0] = '\0';
	if (hwreg_copy(n.parent_id, &parent)) {
		strlcpy(bus, parent.name, sizeof(bus));
		k = strlen(bus);
		while (k > 0 && bus[k - 1] >= '0' && bus[k - 1] <= '9')
			bus[--k] = '\0';
	}
	search_hints(bus[0] != '\0' ? bus : NULL, n.pnpinfo, mod, sizeof(mod));
	if (mod[0] == '\0' || strcmp(mod, "kernel") == 0) {
		*error_code = ENOENT;			/* nothing claims it */
		return KERN_SUCCESS;
	}
	if (kldload(mod) < 0 && errno != EEXIST) {
		*error_code = errno;
		xlog("hwreg_load_driver(%ju): kldload(%s): %s",
		    (uintmax_t)id, mod, strerror(errno));
		return KERN_SUCCESS;
	}
	strlcpy(loaded_module, mod, sizeof(hwreg_name_t));
	xlog("hwreg_load_driver(%ju): module %s loaded", (uintmax_t)id, mod);
	return KERN_SUCCESS;
}

/* hwreg_retain — bump node `id`'s reference count. */
kern_return_t
hwreg_retain(mach_port_t server, uint64_t id)
{
	(void)server;
	if (hwreg_node_retain(id) < 0)
		return KERN_INVALID_ARGUMENT;		/* unknown id */
	return KERN_SUCCESS;
}

/* hwreg_release — drop a reference on node `id`. */
kern_return_t
hwreg_release(mach_port_t server, uint64_t id)
{
	(void)server;
	if (hwreg_node_release(id) < 0)
		return KERN_INVALID_ARGUMENT;		/* unknown id */
	return KERN_SUCCESS;
}

/*
 * Mach service thread. Checks the org.freebsd.hwregd service in with
 * launchd and runs a raw mach_msg receive loop. It is a SECOND thread
 * on purpose — if Mach IPC stalls, the devctl loop on the main thread
 * keeps running. A raw mach_msg loop, not a libdispatch
 * DISPATCH_SOURCE_TYPE_MACH_RECV source: dispatch sources deadlock in
 * this port (task #41). The 500ms receive timeout lets the loop
 * re-check got_term for a clean shutdown. Each message is dispatched
 * either to the pub/sub registry (a SUBSCRIBE) or to the MIG query
 * demux hwreg_server() (a hwreg.defs RPC).
 */
static void *
mach_service_thread(void *arg)
{
	mach_port_t sp = MACH_PORT_NULL;
	kern_return_t kr;

	(void)arg;

	kr = bootstrap_check_in(bootstrap_port, HWREGD_SERVICE_NAME, &sp);
	if (kr != KERN_SUCCESS) {
		xlog("bootstrap_check_in(%s) failed: 0x%x — Mach pub/sub off",
		    HWREGD_SERVICE_NAME, (unsigned)kr);
		return NULL;
	}
	xlog("Mach service '%s' checked in (port=0x%x)",
	    HWREGD_SERVICE_NAME, (unsigned)sp);

	while (!got_term) {
		union {
			mach_msg_header_t hdr;
			char buf[HWREG_RPC_BUFSZ];
		} req, rep;
		mach_msg_return_t mr;

		memset(&req, 0, sizeof(req));
		mr = mach_msg(&req.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
		    0, sizeof(req), sp, 500, MACH_PORT_NULL);
		if (mr == MACH_RCV_TIMED_OUT)
			continue;
		if (mr != MACH_MSG_SUCCESS) {
			xlog("Mach receive failed: 0x%x — Mach service off",
			    (unsigned)mr);
			break;
		}

		if (req.hdr.msgh_id == HWREG_MSG_SUBSCRIBE) {
			mach_port_t client = req.hdr.msgh_remote_port;
			int total = -1;

			pthread_mutex_lock(&subscribers_lock);
			if (n_subscribers < HWREGD_MAX_SUBSCRIBERS) {
				subscribers[n_subscribers++] = client;
				total = n_subscribers;
			}
			pthread_mutex_unlock(&subscribers_lock);

			if (total < 0) {
				xlog("Mach: subscriber table full, dropping 0x%x",
				    (unsigned)client);
			} else {
				xlog("Mach: subscriber 0x%x added (total=%d)",
				    (unsigned)client, total);
				/*
				 * Ack so the client confirms the round-trip.
				 * If the client already vanished the next
				 * publish_event() prunes it.
				 */
				(void)send_event_to(client, 'A',
				    "subscribed to " HWREGD_SERVICE_NAME);
			}
			continue;
		}

		/*
		 * Anything else goes to the MIG query demux: it writes the
		 * routine's reply (or a MIG error) into `rep`. A false
		 * return is a msgh_id this subsystem doesn't recognise.
		 */
		memset(&rep, 0, sizeof(rep));
		if (!hwreg_server(&req.hdr, &rep.hdr)) {
			xlog("Mach: ignoring message id=%d", req.hdr.msgh_id);
			continue;
		}
		if (rep.hdr.msgh_remote_port != MACH_PORT_NULL) {
			mr = mach_msg(&rep.hdr, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
			    rep.hdr.msgh_size, 0, MACH_PORT_NULL, 200,
			    MACH_PORT_NULL);
			if (mr != MACH_MSG_SUCCESS)
				xlog("Mach: RPC reply send failed: 0x%x",
				    (unsigned)mr);
		}
	}
	return NULL;
}

/*
 * PCI property enrichment (Phase 1 iter 4b). Walk /dev/pci via
 * PCIOCGETCONF and attach each device's vendor / device / subvendor /
 * class ids to its registry node, matched by driver name + unit.
 */
static void
enrich_pci(void)
{
	struct pci_conf conf[32];
	struct pci_conf_io io;
	int fd, enriched = 0, restarts = 0;

	fd = open("/dev/pci", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		xlog("hwreg: /dev/pci unavailable (%s) — PCI enrichment skipped",
		    strerror(errno));
		return;
	}
	memset(&io, 0, sizeof(io));
	do {
		unsigned int i;

		io.matches = conf;
		io.match_buf_len = sizeof(conf);
		if (ioctl(fd, PCIOCGETCONF, &io) < 0) {
			xlog("hwreg: PCIOCGETCONF failed: %s", strerror(errno));
			break;
		}
		if (io.status == PCI_GETCONF_LIST_CHANGED) {
			/* The device list moved under us — restart the scan. */
			if (++restarts > 4)
				break;
			memset(&io, 0, sizeof(io));
			enriched = 0;
			continue;
		}
		if (io.status == PCI_GETCONF_ERROR)
			break;
		for (i = 0; i < io.num_matches; i++) {
			char name[32];
			uint32_t cls;

			if (conf[i].pd_name[0] == '\0')
				continue;	/* no driver — no node to match */
			(void)snprintf(name, sizeof(name), "%s%lu",
			    conf[i].pd_name, conf[i].pd_unit);
			cls = ((uint32_t)conf[i].pc_class << 16) |
			    ((uint32_t)conf[i].pc_subclass << 8) |
			    (uint32_t)conf[i].pc_progif;
			if (hwreg_set_pci(name, conf[i].pc_vendor,
			    conf[i].pc_device, conf[i].pc_subvendor, cls))
				enriched++;
		}
	} while (io.status == PCI_GETCONF_MORE_DEVS);
	(void)close(fd);
	xlog("hwreg: PCI enrichment — %d device node(s) enriched", enriched);
}

/* hwreg_foreach() callback — log one registry node. */
static void
log_hw_node(const struct hw_node *n, void *arg)
{
	(void)arg;
	xlog("hwreg: [%2ju<-%2ju] %-16s %-10s %-8s drv=%-8s %s",
	    (uintmax_t)n->id, (uintmax_t)n->parent_id, n->name,
	    n->classname, hw_state_name(n->state),
	    n->driver[0] ? n->driver : "-", n->path);
}

int
main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	xlog("starting (Phase 1 iter 1: hardware registry tree)");

	/* Clean shutdown on SIGTERM / SIGINT (launchd sends SIGTERM). */
	{
		struct sigaction sa = { .sa_handler = on_signal };
		sigemptyset(&sa.sa_mask);
		(void)sigaction(SIGTERM, &sa, NULL);
		(void)sigaction(SIGINT, &sa, NULL);
	}

	read_linker_hints();

	/*
	 * Phase 1 iter 1 — build the hardware registry from a snapshot of
	 * the kernel newbus tree. /dev/devctl attach/detach events keep it
	 * current from here on (see handle_attach_detach).
	 */
	{
		int nnodes = hwreg_build_snapshot();

		if (nnodes < 0)
			xlog("hwreg: registry snapshot failed: %s",
			    strerror(errno));
		else {
			xlog("hwreg: registry built — %d device nodes",
			    nnodes);
			enrich_pci();		/* Phase 1 iter 4b */
			hwreg_foreach(log_hw_node, NULL);
		}
	}

	int fd = open_devctl();
	if (fd < 0)
		return 1;
	xlog("opened %s fd=%d", DEVCTL_PATH, fd);

	/*
	 * Crash-recovery thaw. DEV_FREEZE is a single global kernel bool with
	 * no refcount; if a previous hwregd instance died between
	 * devctl_freeze() and devctl_thaw() (e.g. SIGKILL mid
	 * drain_deferred_loads), the device tree is left frozen, and our own
	 * freeze below would return EBUSY and fall through to unfenced kldload
	 * — the exact recursive-attach wedge the freeze exists to prevent. An
	 * unconditional thaw at startup clears any such stale freeze; thawing
	 * when not frozen is a harmless no-op. launchd KeepAlive respawns us,
	 * so this runs on every (re)start.
	 */
	(void)devctl_thaw();
	xlog("startup devctl_thaw issued (clears any stale freeze from a "
	    "crashed predecessor)");

	/* Mach pub/sub service runs on its own thread (iter 3b-i). */
	{
		pthread_t mth;
		if (pthread_create(&mth, NULL, mach_service_thread,
		    NULL) != 0)
			xlog("pthread_create(mach_service_thread) failed: %s",
			    strerror(errno));
		else
			(void)pthread_detach(mth);
	}

	static char buf[READ_BUF_SIZE];

	/*
	 * Initial backlog drain — replaces the old 250ms wall-clock quiet
	 * window. SYSINIT ordering guarantees the cold-boot device probe
	 * (SI_SUB_INT_CONFIG_HOOKS config-hook drain -> root mount ->
	 * init/launchd) completed before this daemon was launched, so whatever
	 * /dev/devctl has queued right now IS the complete boot backlog. Drain
	 * it to empty, matching '?' nomatches into the deferred-load list
	 * (act_on_match() defers while !live_mode), then do the single
	 * freeze/kldload/thaw batch. This is an event precondition, not a
	 * timer: the continuous '!notify DEVFS CDEV CREATE' stream that used to
	 * starve the old quiet window (boot stalled ~60s on real hardware, #67)
	 * is irrelevant — we drain far faster than it arrives, so the queue
	 * empties promptly. Late/hot-plug devices (e.g. async USB) arrive after
	 * the flip and are kldload'd inline in live mode.
	 *
	 * "Queue empty" is detected with a zero-timeout select() poll, NOT a
	 * non-blocking read()==EAGAIN: /dev/devctl does not honor O_NONBLOCK on
	 * its read (read() blocks on an empty queue), so select() readability —
	 * the same signal the live loop uses — is the portable "anything left
	 * to drain?" test.
	 */
	while (!got_term) {
		fd_set rfds;
		struct timeval poll = { .tv_sec = 0, .tv_usec = 0 };
		int r;

		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		r = select(fd + 1, &rfds, NULL, NULL, &poll);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			xlog("select failed: %s", strerror(errno));
			(void)close(fd);
			return 1;
		}
		if (r == 0)		/* nothing ready — kernel queue drained */
			break;
		if (drain_devctl(fd, buf, sizeof(buf)) < 0) {
			(void)close(fd);
			return 1;
		}
	}
	if (!got_term) {
		live_mode = true;
		xlog("devctl backlog drained to empty — live mode: freeze + "
		    "drain queued kldloads + thaw");
		drain_deferred_loads();
	}

	/*
	 * Live mode — react to hot-plug events. The cold-boot probe is over
	 * (backlog drained above), so each hot-plug '?' nomatch is kldload'd
	 * inline by act_on_match(). select() with a 5s idle tick; got_term
	 * (SIGTERM/SIGINT from launchd) ends the loop.
	 */
	while (!got_term) {
		fd_set rfds;
		struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
		int r;

		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		r = select(fd + 1, &rfds, NULL, NULL, &tv);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			xlog("select failed: %s", strerror(errno));
			break;
		}
		if (r > 0 && FD_ISSET(fd, &rfds)) {
			if (drain_devctl(fd, buf, sizeof(buf)) < 0)
				break;
		}
	}

	xlog("shutting down (signal=%d)", (int)got_term);
	(void)close(fd);
	return 0;
}
