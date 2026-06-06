/*
 * kextd — push kext IOKitPersonalities into the in-kernel IOCatalogue (U1,
 * nextbsd#217).
 *
 * The Apple-faithful loader half of the in-kernel IOKit matcher (umbrella
 * #211, Option B, mechanism (a)): userland parses each kext bundle's
 * IOKitPersonalities and PUSHES them to the kernel; the kernel (K2 #215 store,
 * K3 #216 matcher) owns matching. This is the analog of Apple kextd's boot-time
 * IOCatalogueSendData.
 *
 * What it does now (push): open every kext under a repository (default
 * /System/Library/Extensions) with the OSKext engine, take each personality's
 * device-id match table (IOProviderClass / IOProbeScore / IOPCIPrimaryMatch),
 * and submit one flat record per personality to /dev/iocatalogue via
 * IOCATIOCADD. It first IOCATIOCFLUSHes, so running it again simply re-pushes
 * the current set (idempotent). The kernel never parses XML — kextd does.
 *
 * Invoked on demand for now (e.g. by the on-image IOKit test). The launchd
 * boot-time auto-push is deferred to K3 (#216): a RunAtLoad CF/OSKext job this
 * early wedges launchd's boot dispatch, so kextd grows its boot integration
 * once it is a persistent daemon whose ordering is designed.
 *
 * Not yet (lands with K3, #216): listening on /dev/devctl for the kernel's
 * "IOKIT load <bundle-id>" requests and kextload-ing the named bundle. For now
 * kextd populates the catalogue and exits; the matcher that consumes it is K3.
 *
 * Usage:  kextd [-r repo_dir] [-v]
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_types.h>	/* kern_return_t, before OSKext.h -> OSReturn.h */
#include <mach/mach_traps.h>	/* mach_task_self, mach_host_self, mach_msg */
#include <mach/mach_port.h>	/* mach_port_allocate, mach_port_insert_right */
#include <mach/host_special_ports.h>	/* host_set_special_port, HOST_KEXTD_PORT */
#include <mach/message.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "OSKext.h"
#include "iocatalogue.h"	/* vendored ABI; canonical copy in nextbsd-kernel */

#ifndef HOST_KEXTD_PORT
#define HOST_KEXTD_PORT 15
#endif

/* kernel->kextd load request wire format (mirror of the kernel's
 * iokit_kextd_load_msg_t in sys/mach/iokit_kextd.h; NDR_record_t is 8 bytes). */
#define	IOKIT_KEXTD_LOAD_MSGID	0x494f4b54
typedef struct {
	mach_msg_header_t	hdr;
	unsigned char		ndr[8];
	char			bundle_id[128];
	char			device[64];
	uint32_t		match_word;
} kextd_load_body_t;

static bool verbose;

static void
tool_log(OSKextRef aKext __unused, OSKextLogSpec spec __unused,
    const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

static CFURLRef
url_for_path(const char *path)
{
	return CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
	    (const UInt8 *)path, (CFIndex)strlen(path), true);
}

/* Copy a CFString value into a C buffer; returns false if absent/not a string. */
static bool
dict_string(CFDictionaryRef d, CFStringRef key, char *buf, size_t buflen)
{
	CFStringRef s = CFDictionaryGetValue(d, key);

	if (s == NULL || CFGetTypeID(s) != CFStringGetTypeID())
		return (false);
	return (CFStringGetCString(s, buf, (CFIndex)buflen,
	    kCFStringEncodingUTF8) != false);
}

/* Fetch an integer CFNumber value; returns def if absent/not a number. */
static int32_t
dict_int(CFDictionaryRef d, CFStringRef key, int32_t def)
{
	CFNumberRef n = CFDictionaryGetValue(d, key);
	int32_t v;

	if (n == NULL || CFGetTypeID(n) != CFNumberGetTypeID())
		return (def);
	if (!CFNumberGetValue(n, kCFNumberSInt32Type, &v))
		return (def);
	return (v);
}

/*
 * Parse an IOPCIPrimaryMatch string ("0x24f38086 0x...") into a freshly
 * malloc'd array of match words. Returns the count (0 on none/absent); *out is
 * set to the array (caller frees) or NULL.
 */
static uint32_t
parse_match(CFDictionaryRef d, uint32_t **out)
{
	char buf[8192];
	uint32_t *words;
	uint32_t n = 0;
	char *p, *tok, *save;

	*out = NULL;
	if (!dict_string(d, CFSTR("IOPCIPrimaryMatch"), buf, sizeof(buf)))
		return (0);

	words = calloc(IOCAT_MAX_MATCH, sizeof(*words));
	if (words == NULL)
		return (0);
	for (p = buf; (tok = strtok_r(p, " ,\t", &save)) != NULL; p = NULL) {
		if (n >= IOCAT_MAX_MATCH)
			break;
		words[n++] = (uint32_t)strtoul(tok, NULL, 16);
	}
	if (n == 0) {
		free(words);
		return (0);
	}
	*out = words;
	return (n);
}

/* Push one personality dict; returns 1 if pushed, 0 if skipped, -1 on error. */
static int
push_personality(int fd, CFDictionaryRef p)
{
	struct iocat_add add;
	char provider[64];
	uint32_t *words;
	uint32_t n;
	int rc;

	memset(&add, 0, sizeof(add));
	if (!dict_string(p, CFSTR("CFBundleIdentifier"), add.bundle_id,
	    sizeof(add.bundle_id)))
		return (0);	/* no bundle id -> can't be loaded; skip */

	if (dict_string(p, CFSTR("IOProviderClass"), provider, sizeof(provider)) &&
	    strcmp(provider, "IOPCIDevice") == 0)
		add.provider_class = IOCAT_PROVIDER_IOPCIDEVICE;
	else
		add.provider_class = IOCAT_PROVIDER_UNKNOWN;

	n = parse_match(p, &words);
	if (n == 0)
		return (0);	/* no device-id match table -> nothing to match */

	add.probe_score = dict_int(p, CFSTR("IOProbeScore"), 0);
	add.nmatch = n;
	add.match = (uint64_t)(uintptr_t)words;

	rc = ioctl(fd, IOCATIOCADD, &add);
	free(words);
	if (rc != 0) {
		warn("IOCATIOCADD %s", add.bundle_id);
		return (-1);
	}
	if (verbose)
		printf("kextd: pushed %s (%s, score %d, %u ids)\n",
		    add.bundle_id,
		    add.provider_class == IOCAT_PROVIDER_IOPCIDEVICE ?
		    "IOPCIDevice" : "?", add.probe_score, n);
	return (1);
}

/*
 * -l <matchword>: ask the kernel which driver bundle claims a PCI id
 * (0x<device><vendor>) via IOCATIOCLOOKUP — the same lookup the in-kernel
 * device_nomatch matcher (K3) uses. Lets the matcher be verified deterministically
 * (e.g. 0x24f38086 -> org.nextbsd.kext.intelwifi) without the physical device.
 */
static int
do_lookup(const char *word)
{
	struct iocat_lookup lu;
	int fd, rc;

	memset(&lu, 0, sizeof(lu));
	lu.match = (uint32_t)strtoul(word, NULL, 0);
	fd = open("/dev/iocatalogue", O_RDWR);
	if (fd < 0)
		err(1, "open /dev/iocatalogue (is the K2 kernel loaded?)");
	rc = ioctl(fd, IOCATIOCLOOKUP, &lu);
	close(fd);
	if (rc == 0)
		printf("LOOKUP 0x%08x -> %s score %d\n", lu.match,
		    lu.bundle_id, lu.score);
	else if (errno == ENOTTY)	/* kernel without K3a's IOCATIOCLOOKUP */
		printf("LOOKUP 0x%08x -> unsupported\n", lu.match);
	else
		printf("LOOKUP 0x%08x -> (none)\n", lu.match);
	return (rc == 0 ? 0 : 1);
}

/* Load a kext by CFBundleIdentifier from the already-open repo set. */
static void
load_bundle(const char *bundle_id)
{
	CFStringRef idstr;
	OSKextRef k;
	OSReturn r;

	idstr = CFStringCreateWithCString(kCFAllocatorDefault, bundle_id,
	    kCFStringEncodingUTF8);
	if (idstr == NULL)
		return;
	k = OSKextGetKextWithIdentifier(idstr);	/* from the open repo set */
	CFRelease(idstr);
	if (k == NULL) {
		printf("kextd: no kext with identifier %s\n", bundle_id);
		return;
	}
	r = OSKextLoad(k);
	if (r == kOSReturnSuccess)
		printf("kextd: loaded %s\n", bundle_id);
	else {
		printf("kextd: load %s failed (OSReturn 0x%x)\n", bundle_id,
		    (unsigned)r);
		OSKextLogDiagnostics(k, kOSKextDiagnosticsFlagAll);
	}
}

/*
 * Watch mode (the real daemon, U1/K3b): register HOST_KEXTD_PORT so the kernel
 * matcher can reach us, push the repo's personalities into the IOCatalogue
 * (which, with push-triggers-match, fires load requests for already-unmatched
 * devices), then serve kernel load requests forever — loading each named bundle
 * via OSKext (kldload re-probes and the device attaches). No matching here.
 */
static int
do_watch(int fd, const char *repo)
{
	mach_port_name_t task = mach_task_self();
	mach_port_name_t host = mach_host_self();
	mach_port_name_t port = MACH_PORT_NULL;
	CFArrayRef repoKexts, personalities;
	CFURLRef u;
	kern_return_t kr;
	union {
		kextd_load_body_t body;
		unsigned char raw[sizeof(kextd_load_body_t) + 64];
	} buf;

	kr = mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &port);
	if (kr != KERN_SUCCESS)
		errx(1, "mach_port_allocate: 0x%x", (unsigned)kr);
	kr = mach_port_insert_right(task, port, port, MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS)
		errx(1, "mach_port_insert_right: 0x%x", (unsigned)kr);
	kr = host_set_special_port(host, HOST_KEXTD_PORT, port);
	if (kr != KERN_SUCCESS)
		errx(1, "host_set_special_port(HOST_KEXTD_PORT): 0x%x", (unsigned)kr);
	/* Startup step markers are verbose-only — under -v they bracket each
	 * call so a hang's last line pinpoints the blocker; at default the
	 * daemon is quiet and logs only meaningful events (loaded, errors). */
	if (verbose)
		printf("kextd: listening on HOST_KEXTD_PORT (port 0x%x)\n", port);

	if (verbose)
		printf("kextd: opening repo %s\n", repo);
	u = url_for_path(repo);
	repoKexts = (u != NULL) ?
	    OSKextCreateKextsFromURL(kCFAllocatorDefault, u) : NULL;
	if (u != NULL)
		CFRelease(u);
	if (repoKexts == NULL)
		errx(1, "%s: no kexts found", repo);
	if (verbose)
		printf("kextd: repo opened; copying personalities\n");
	(void) ioctl(fd, IOCATIOCFLUSH);
	personalities = OSKextCopyPersonalitiesOfKexts(NULL);
	if (personalities != NULL) {
		CFIndex n = CFArrayGetCount(personalities), i;
		int pushed = 0;

		for (i = 0; i < n; i++) {
			CFDictionaryRef p = CFArrayGetValueAtIndex(personalities, i);
			if (p != NULL && CFGetTypeID(p) == CFDictionaryGetTypeID() &&
			    push_personality(fd, p) > 0)
				pushed++;
		}
		if (verbose)
			printf("kextd: pushed %d personalities\n", pushed);
		CFRelease(personalities);
	}

	if (verbose)
		printf("kextd: ready\n");
	for (;;) {
		mach_msg_return_t mr;

		memset(&buf, 0, sizeof(buf));
		mr = mach_msg(&buf.body.hdr, MACH_RCV_MSG, 0, sizeof(buf), port,
		    MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
		if (mr != MACH_MSG_SUCCESS) {
			fprintf(stderr, "kextd: mach_msg(RCV) 0x%x\n", (unsigned)mr);
			continue;
		}
		if (buf.body.hdr.msgh_id != IOKIT_KEXTD_LOAD_MSGID)
			continue;
		buf.body.bundle_id[sizeof(buf.body.bundle_id) - 1] = '\0';
		if (verbose)
			printf("kextd: load request bundle=%s device=%s match=0x%08x\n",
			    buf.body.bundle_id, buf.body.device, buf.body.match_word);
		load_bundle(buf.body.bundle_id);
	}
	/* NOTREACHED */
}

/* -t <word>: drive the kernel matcher's send for a PCI id (IOCATIOCTESTSEND),
 * WITHOUT registering HOST_KEXTD_PORT — so it injects a request to a separately
 * running `kextd -w`. CI uses this to exercise the daemon's receive+load. */
static int
do_test_send(int fd, const char *word)
{
	uint32_t mw = (uint32_t)strtoul(word, NULL, 0);
	int rc = ioctl(fd, IOCATIOCTESTSEND, &mw);

	printf("kextd: test-send 0x%08x rc=%d errno=%d\n", mw, rc,
	    rc != 0 ? errno : 0);
	return (rc == 0 ? 0 : 1);
}

int
main(int argc, char *argv[])
{
	const char *repo = "/System/Library/Extensions";
	const char *lookup = NULL;
	const char *testsend = NULL;
	bool watch = false;
	CFArrayRef repoKexts = NULL;	/* non-retaining registry: hold alive */
	CFArrayRef personalities = NULL;
	CFURLRef u;
	int fd, ch;
	int pushed = 0, skipped = 0, failed = 0;
	CFIndex i, count;

	/* Line-buffer stdout: in -w (daemon) mode our output is redirected to a
	 * file (fully buffered), and the test kill()s us — buffered diagnostics
	 * would be lost. Line buffering flushes each message as it's printed. */
	setlinebuf(stdout);

	while ((ch = getopt(argc, argv, "r:vl:wt:")) != -1) {
		switch (ch) {
		case 'r':
			repo = optarg;
			break;
		case 'v':
			verbose = true;
			break;
		case 'l':
			lookup = optarg;
			break;
		case 'w':
			watch = true;
			break;
		case 't':
			testsend = optarg;
			break;
		default:
			errx(2, "usage: kextd [-r repo] [-v] | -l word | -w | -t word");
		}
	}

	/* Query mode: just resolve one PCI id against the catalogue and exit. */
	if (lookup != NULL)
		return (do_lookup(lookup));

	OSKextSetLogOutputFunction(&tool_log);
	if (verbose)
		OSKextSetLogFilter(kOSKextLogDetailLevel |
		    kOSKextLogVerboseFlagsMask, false);

	fd = open("/dev/iocatalogue", O_RDWR);
	if (fd < 0)
		err(1, "open /dev/iocatalogue (is the K2 kernel loaded?)");

	/* Trigger a synthetic load request to a running `kextd -w` (CI helper). */
	if (testsend != NULL) {
		int rc = do_test_send(fd, testsend);
		close(fd);
		return (rc);
	}

	/* Watch mode: register HOST_KEXTD_PORT, push, then serve load requests. */
	if (watch)
		return (do_watch(fd, repo));	/* does not return */

	/* Idempotent: drop the prior set, then re-push the current repo. */
	if (ioctl(fd, IOCATIOCFLUSH) != 0)
		warn("IOCATIOCFLUSH");

	u = url_for_path(repo);
	if (u == NULL)
		errx(1, "%s: cannot form URL", repo);
	repoKexts = OSKextCreateKextsFromURL(kCFAllocatorDefault, u);
	CFRelease(u);
	if (repoKexts == NULL)
		errx(1, "%s: no kexts found", repo);

	/* All personalities across all open kexts, processed for the kernel. */
	personalities = OSKextCopyPersonalitiesOfKexts(NULL);
	if (personalities == NULL)
		errx(1, "no personalities in %s", repo);

	count = CFArrayGetCount(personalities);
	for (i = 0; i < count; i++) {
		CFDictionaryRef p = CFArrayGetValueAtIndex(personalities, i);
		int r;

		if (p == NULL || CFGetTypeID(p) != CFDictionaryGetTypeID())
			continue;
		r = push_personality(fd, p);
		if (r > 0)
			pushed++;
		else if (r == 0)
			skipped++;
		else
			failed++;
	}

	printf("kextd: %d personalities pushed, %d skipped, %d failed (repo %s)\n",
	    pushed, skipped, failed, repo);

	CFRelease(personalities);
	CFRelease(repoKexts);
	close(fd);
	return (failed > 0 ? 1 : 0);
}
