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
 * `?` events buffered from early boot. kldload'ing for those wedged
 * the CI boot test — a matched driver's attach() then runs mid-boot
 * under kernel locks (the CI VM's offender was the ICH SMBus
 * controller -> ichsmb). So hwregd kldloads only for *live* events:
 * for HWREGD_SETTLE_SECONDS after startup it match-and-logs every
 * nomatch, then flips to live mode and kldloads on each subsequent
 * nomatch. The window is time-based, not "until devctl goes quiet" —
 * a chronically noisy devctl (e.g. a flaky drive spamming CAM error
 * events) must not pin hwregd in backlog mode forever. Boot-time
 * drivers are the loader's job; hot-plug is hwregd's.
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
#include <sys/linker.h>
#include <sys/module.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

#include <ctype.h>
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

#define DEVCTL_PATH		"/dev/devctl"
#define READ_BUF_SIZE		(64 * 1024)
#define EVENT_LINE_MAX		1024

/*
 * Boot-settle window. hwregd match-and-logs nomatch events for this
 * many seconds after startup, then flips to live mode and kldloads.
 * Long enough to outlast the kernel's boot-time device probe so an
 * autoload never runs a driver attach mid-boot.
 */
#define HWREGD_SETTLE_SECONDS	60

/* Mach service name hwregd checks in with launchd for its pub/sub bus. */
#define HWREGD_SERVICE_NAME	"org.freebsd.hwregd"

/*
 * Mach pub/sub wire protocol (iter 3b-ii). A client looks the service
 * up, sends a bare-header SUBSCRIBE (its own notify port travels as
 * the message's remote port), and then receives EVENT messages.
 */
#define HWREG_MSG_SUBSCRIBE	0x48575201	/* client  -> hwregd */
#define HWREG_MSG_EVENT		0x48575202	/* hwregd   -> subscriber */

/* The event message hwregd pushes to each subscriber. */
struct hwreg_event_msg {
	mach_msg_header_t	hdr;
	char			kind;	/* '+' '-' '!' '?' or 'A' (sub ack) */
	char			text[479];	/* formatted event, NUL-term */
};

#define HWREGD_MAX_SUBSCRIBERS	32

static volatile sig_atomic_t got_term = 0;

/*
 * Subscriber registry. The Mach thread appends on SUBSCRIBE; the
 * devctl (main) thread reads it to publish. Guarded by the mutex.
 */
static mach_port_t	subscribers[HWREGD_MAX_SUBSCRIBERS];
static int		n_subscribers;
static pthread_mutex_t	subscribers_lock = PTHREAD_MUTEX_INITIALIZER;

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
 * false for the first HWREGD_SETTLE_SECONDS of uptime (the boot-
 * settle window). hwregd kldloads only in live mode; matches during
 * the window are logged but not loaded — see act_on_match().
 */
static bool live_mode = false;

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
 * Act on a module that linker.hints says claims a nomatched device.
 * "kernel" is the pseudo-module for built-in drivers — nothing to
 * load. In live mode (hot-plug) the module is kldload(2)ed; during
 * the boot backlog it is only logged — kldloading mid-boot wedges
 * the boot (a driver attach runs under kernel locks; see the file
 * header). EEXIST (already loaded) is a normal, expected outcome.
 */
static void
act_on_match(const char *mod)
{
	if (mod == NULL || mod[0] == '\0' || strcmp(mod, "kernel") == 0)
		return;

	if (!live_mode) {
		xlog("match: module '%s' claims this device "
		    "(boot backlog — not loaded)", mod);
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
 * devmatch.c's search_hints(); on a match, kldload the module
 * instead of printing it. The dump_flag / verbose_flag branches are
 * dead (compile-time false) but kept so the port stays verbatim.
 */
static void
search_hints(const char *bus, const char *pnpinfo)
{
	char val1[256], val2[256];
	int ival, len, ents, i, notme, mask, bit, v, found;
	void *ptr, *walker;
	char *lastmod = NULL, *cp, *s;

	walker = hints;
	getint(&walker);
	found = 0;
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
				if (!dump_flag && !notme) {
					act_on_match(lastmod);
					found++;
				}
			}
			break;
		default:
			break;
		}
		walker = (void *)(len - sizeof(int) + (intptr_t)walker);
	}
	if (found == 0)
		xlog("nomatch: no module in linker.hints claims '%s'",
		    pnpinfo);
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
	search_hints(bus, pnpinfo);
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
}

/* --- devctl event loop -------------------------------------------- */

static int
open_devctl(void)
{
	int fd = open(DEVCTL_PATH, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		xlog("open(%s) failed: %s", DEVCTL_PATH, strerror(errno));
	return fd;
}

/*
 * Read whatever's pending on /dev/devctl, split into '\n'-terminated
 * lines, and dispatch each to the parser for its event class.
 * Returns bytes read, -1 on a fatal read error, 0 on retry/EOF.
 */
static ssize_t
drain_devctl(int fd, char *buf, size_t bufsz)
{
	ssize_t n = read(fd, buf, bufsz - 1);
	if (n < 0) {
		if (errno == EINTR || errno == EAGAIN)
			return 0;
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

/*
 * Mach service thread (iter 3b-i skeleton). Checks the
 * org.freebsd.hwregd service in with launchd and runs a raw mach_msg
 * receive loop. It is a SECOND thread on purpose — if Mach IPC
 * stalls, the devctl loop on the main thread keeps running. A raw
 * mach_msg loop, not a libdispatch DISPATCH_SOURCE_TYPE_MACH_RECV
 * source: dispatch sources deadlock in this port (task #41). The
 * 500ms receive timeout lets the loop re-check got_term for a clean
 * shutdown. iter 3b-ii adds the subscribe/publish protocol; for now
 * any received message is just logged.
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
		struct {
			mach_msg_header_t hdr;
			uint8_t body[512];
		} msg;
		mach_msg_return_t mr;

		memset(&msg, 0, sizeof(msg));
		mr = mach_msg(&msg.hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
		    0, sizeof(msg), sp, 500, MACH_PORT_NULL);
		if (mr == MACH_RCV_TIMED_OUT)
			continue;
		if (mr != MACH_MSG_SUCCESS) {
			xlog("Mach receive failed: 0x%x — Mach pub/sub off",
			    (unsigned)mr);
			break;
		}
		if (msg.hdr.msgh_id == HWREG_MSG_SUBSCRIBE) {
			mach_port_t client = msg.hdr.msgh_remote_port;
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
				 * publish_event() prunes it — no need to act
				 * on the result here.
				 */
				(void)send_event_to(client, 'A',
				    "subscribed to " HWREGD_SERVICE_NAME);
			}
		} else {
			xlog("Mach: ignoring message id=%d", msg.hdr.msgh_id);
		}
	}
	return NULL;
}

int
main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	xlog("starting (Phase 0 iter 3b-ii: Mach pub/sub bus)");

	/* Clean shutdown on SIGTERM / SIGINT (launchd sends SIGTERM). */
	{
		struct sigaction sa = { .sa_handler = on_signal };
		sigemptyset(&sa.sa_mask);
		(void)sigaction(SIGTERM, &sa, NULL);
		(void)sigaction(SIGINT, &sa, NULL);
	}

	read_linker_hints();

	int fd = open_devctl();
	if (fd < 0)
		return 1;
	xlog("opened %s fd=%d", DEVCTL_PATH, fd);

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
	time_t started = time(NULL);

	while (!got_term) {
		/*
		 * Flip to live mode once the boot-settle window has elapsed.
		 * Checked every loop iteration (events or the 5s select
		 * timeout), so it fires within 5s of the deadline.
		 */
		if (!live_mode &&
		    time(NULL) - started >= HWREGD_SETTLE_SECONDS) {
			live_mode = true;
			xlog("boot-settle window (%ds) elapsed — live mode: "
			    "hot-plug autoload enabled", HWREGD_SETTLE_SECONDS);
		}

		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);

		struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
		int r = select(fd + 1, &rfds, NULL, NULL, &tv);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			xlog("select failed: %s", strerror(errno));
			break;
		}
		if (r == 0)
			continue;	/* idle tick — settle check is at loop top */
		if (FD_ISSET(fd, &rfds)) {
			ssize_t n = drain_devctl(fd, buf, sizeof(buf));
			if (n < 0)
				break;
		}
	}

	xlog("shutting down (signal=%d)", (int)got_term);
	(void)close(fd);
	return 0;
}
