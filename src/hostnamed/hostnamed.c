/*
 * hostnamed — freebsd-launchd-mach hostname synthesis + publish daemon.
 *
 * iter 1: one-shot daemon. Synthesizes a hostname at boot and publishes
 * it three ways:
 *   1. SCDynamicStore "Setup:/System"          (ComputerName)
 *   2. SCDynamicStore "Setup:/Network/HostNames" (HostName + LocalHostName)
 *   3. sethostname(2) + notify_post("com.apple.system.hostname")
 *
 * Replaces FreeBSD's default-unset "Amnesiac" placeholder on machines
 * where /etc/rc.conf hostname= never fires (our launchd boot skips rc).
 *
 * 3-tier synthesis precedence:
 *   T1: kenv "hostname.override"
 *   T2: synthesized "${slug}-${suffix}":
 *         slug   = smbios.system.version | smbios.system.product | "freebsd"
 *         suffix = last-6 alnum of smbios.system.serial   (skip placeholders)
 *               | last-6 hex of first non-loopback NIC MAC
 *               | first 6 hex of kern.hostuuid
 *   T3: bare "freebsd" (final fallback; impossible to reach in practice
 *       since hostuuid is always set).
 *
 * Plan: https://pkgdemon.github.io/freebsd-hostnamed-plan.html
 * Issue: #63
 */

#include <SystemConfiguration/SCDynamicStore.h>
#include <SystemConfiguration/SCPreferences.h>
#include <CoreFoundation/CoreFoundation.h>

#include <dispatch/dispatch.h>

#include <dns_sd.h>

#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>

#include <netinet/in.h>

#include <ctype.h>
#include <errno.h>
#include <ifaddrs.h>
#include <kenv.h>
#include <limits.h>
#include <notify.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HOSTNAMED_MAX	64	/* gethostname(3) limit incl. NUL */
#define SLUG_MAX	40
#define SUFFIX_LEN	6

/* SCDynamicStore key strings. Apple's libSystemConfiguration normally
 * provides SCDynamicStoreKeyCreateComputerName / KeyCreateHostNames
 * helpers; the in-tree port doesn't ship them yet (plan §11 Q7), so we
 * spell the canonical paths directly. Both are stable Apple ABI: the
 * keys mDNSResponder + Setup Assistant + every SC-aware daemon read. */
#define SC_KEY_SYSTEM		"Setup:/System"
#define SC_KEY_HOSTNAMES	"Setup:/Network/HostNames"

static void
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

/* CFStringCreateWithCString wrapper, UTF-8. Same as sc_publish.c. */
static CFStringRef
mkstr(const char *s)
{
	return (CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8));
}

/* Read a kenv key into out (NUL-terminated). Returns length or -1. */
static int
read_kenv(const char *name, char *out, size_t outsz)
{
	int n;

	if (outsz == 0 || outsz > INT_MAX)
		return (-1);
	n = kenv(KENV_GET, name, out, (int)outsz);
	if (n <= 0)
		return (-1);
	/* kenv(2) NUL-terminates on success; defensive cap. */
	out[outsz - 1] = '\0';
	return (n);
}

/* Read a sysctlbyname string into out. Returns length or -1. */
static int
read_sysctl_str(const char *name, char *out, size_t outsz)
{
	size_t n = outsz;

	if (sysctlbyname(name, out, &n, NULL, 0) != 0 || n == 0)
		return (-1);
	out[outsz - 1] = '\0';
	if (n > outsz)
		n = outsz;
	return ((int)n);
}

/* Sanitize a slug in place: trim, collapse non-[A-Za-z0-9] runs to '-',
 * strip leading/trailing '-', truncate to SLUG_MAX. Returns new length. */
static size_t
sanitize_slug(char *s)
{
	size_t i, j;
	int prev_dash;

	if (s == NULL)
		return (0);

	/* In-place collapse. */
	j = 0;
	prev_dash = 1; /* suppress leading '-' */
	for (i = 0; s[i] != '\0' && j < SLUG_MAX; i++) {
		unsigned char c = (unsigned char)s[i];
		if (isalnum(c)) {
			s[j++] = (char)c;
			prev_dash = 0;
		} else if (!prev_dash) {
			s[j++] = '-';
			prev_dash = 1;
		}
	}
	/* Strip trailing '-'. */
	while (j > 0 && s[j - 1] == '-')
		j--;
	s[j] = '\0';
	return (j);
}

/* Placeholder-serial detector. Returns 1 if the SMBIOS serial is one of
 * the well-known firmware no-op values that vendors leave when they
 * forget to flash a real serial. Plan §3.T2. */
static int
serial_is_placeholder(const char *s)
{
	static const char *const placeholders[] = {
		"",
		"None",
		"To be filled by O.E.M.",
		"To Be Filled By O.E.M.",
		"Default string",
		"0",
		"0123456789",
		"System Serial Number",
		"Not Specified",
		NULL,
	};
	int i;

	if (s == NULL)
		return (1);
	for (i = 0; placeholders[i] != NULL; i++) {
		if (strcmp(s, placeholders[i]) == 0)
			return (1);
	}
	return (0);
}

/* Copy the last SUFFIX_LEN alphanumeric chars of src into out (NUL-term).
 * Returns 0 on success (suffix is exactly SUFFIX_LEN chars), -1 otherwise. */
static int
last_alnum_suffix(const char *src, char *out)
{
	char filtered[128];
	size_t i, j, n;

	j = 0;
	for (i = 0; src[i] != '\0' && j < sizeof(filtered) - 1; i++) {
		if (isalnum((unsigned char)src[i]))
			filtered[j++] = src[i];
	}
	filtered[j] = '\0';
	if (j < SUFFIX_LEN)
		return (-1);
	n = j - SUFFIX_LEN;
	(void)memcpy(out, filtered + n, SUFFIX_LEN);
	out[SUFFIX_LEN] = '\0';
	return (0);
}

/* Walk getifaddrs(AF_LINK), copy the first non-loopback Ethernet MAC's
 * 6 bytes into mac. Returns 0 on success, -1 on no candidate. */
static int
first_nic_mac(uint8_t mac[6])
{
	struct ifaddrs *ifa, *p;
	int ok = -1;

	if (getifaddrs(&ifa) != 0)
		return (-1);
	for (p = ifa; p != NULL; p = p->ifa_next) {
		const struct sockaddr_dl *dl;
		int zero;
		size_t i;

		if (p->ifa_addr == NULL ||
		    p->ifa_addr->sa_family != AF_LINK)
			continue;
		dl = (const struct sockaddr_dl *)(const void *)p->ifa_addr;
		if (dl->sdl_type != IFT_ETHER || dl->sdl_alen != 6)
			continue;
		zero = 1;
		for (i = 0; i < 6; i++) {
			if (((const uint8_t *)LLADDR(dl))[i] != 0) {
				zero = 0;
				break;
			}
		}
		if (zero)
			continue;
		(void)memcpy(mac, LLADDR(dl), 6);
		ok = 0;
		break;
	}
	freeifaddrs(ifa);
	return (ok);
}

/* Derive the per-machine suffix per plan §3.T2 precedence chain. */
static int
derive_suffix(char out[SUFFIX_LEN + 1])
{
	char buf[256];
	uint8_t mac[6];

	/* Tier A: SMBIOS serial last-6-alnum (skip placeholders). */
	if (read_kenv("smbios.system.serial", buf, sizeof(buf)) > 0 &&
	    !serial_is_placeholder(buf) &&
	    last_alnum_suffix(buf, out) == 0) {
		xlog("suffix from smbios.system.serial='%s' -> '%s'",
		    buf, out);
		return (0);
	}

	/* Tier B: first non-loopback NIC MAC last-6-hex. */
	if (first_nic_mac(mac) == 0) {
		(void)snprintf(out, SUFFIX_LEN + 1, "%02x%02x%02x",
		    mac[3], mac[4], mac[5]);
		xlog("suffix from NIC MAC %02x:%02x:%02x:%02x:%02x:%02x -> '%s'",
		    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], out);
		return (0);
	}

	/* Tier C: first 6 hex chars of kern.hostuuid. */
	if (read_sysctl_str("kern.hostuuid", buf, sizeof(buf)) > 0) {
		char filtered[64];
		size_t i, j;

		j = 0;
		for (i = 0; buf[i] != '\0' &&
		    j < sizeof(filtered) - 1; i++) {
			unsigned char c = (unsigned char)buf[i];
			if (isxdigit(c))
				filtered[j++] = (char)tolower(c);
		}
		filtered[j] = '\0';
		if (j >= SUFFIX_LEN) {
			(void)memcpy(out, filtered, SUFFIX_LEN);
			out[SUFFIX_LEN] = '\0';
			xlog("suffix from kern.hostuuid='%s' -> '%s'",
			    buf, out);
			return (0);
		}
	}

	return (-1);
}

/* Derive the model slug per plan §3.T2 precedence chain. */
static void
derive_slug(char *out, size_t outsz)
{
	char buf[256];

	if (read_kenv("smbios.system.version", buf, sizeof(buf)) > 0) {
		(void)strncpy(out, buf, outsz - 1);
		out[outsz - 1] = '\0';
		(void)sanitize_slug(out);
		if (out[0] != '\0') {
			xlog("slug from smbios.system.version='%s' -> '%s'",
			    buf, out);
			return;
		}
	}
	if (read_kenv("smbios.system.product", buf, sizeof(buf)) > 0) {
		(void)strncpy(out, buf, outsz - 1);
		out[outsz - 1] = '\0';
		(void)sanitize_slug(out);
		if (out[0] != '\0') {
			xlog("slug from smbios.system.product='%s' -> '%s'",
			    buf, out);
			return;
		}
	}
	(void)strncpy(out, "freebsd", outsz - 1);
	out[outsz - 1] = '\0';
	xlog("slug fallback -> 'freebsd'");
}

/* Whitelist + normalize a candidate hostname in-place. Allows
 * [A-Za-z0-9 _.-]{1,253}; replaces spaces with '-' so the result is a
 * valid kernel hostname. Returns 1 if the value passed validation and
 * is now stored in out (NUL-terminated); 0 if rejected (out is
 * untouched). Shared by Tier 1 (kenv override) and Tier 2 (SCPrefs
 * ComputerName) so both apply the same gate. */
static int
validate_and_normalize(const char *src, const char *source_label,
    char *out, size_t outsz)
{
	char buf[256];
	size_t i, len;

	if (src == NULL)
		return (0);
	len = strlen(src);
	if (len == 0 || len > sizeof(buf) - 1 || len > 253) {
		xlog("%s rejected: bad length %zu", source_label, len);
		return (0);
	}
	(void)memcpy(buf, src, len);
	buf[len] = '\0';
	for (i = 0; buf[i] != '\0'; i++) {
		unsigned char c = (unsigned char)buf[i];
		if (!isalnum(c) && c != ' ' && c != '_' &&
		    c != '.' && c != '-') {
			xlog("%s rejected: invalid char 0x%02x",
			    source_label, (unsigned)c);
			return (0);
		}
	}
	for (i = 0; buf[i] != '\0'; i++) {
		if (buf[i] == ' ')
			buf[i] = '-';
	}
	(void)strncpy(out, buf, outsz - 1);
	out[outsz - 1] = '\0';
	return (1);
}

/* Tier 1: kenv hostname.override (operator pre-boot pin). */
static int
try_override(char *out, size_t outsz)
{
	char buf[256];

	if (read_kenv("hostname.override", buf, sizeof(buf)) <= 0)
		return (0);
	if (!validate_and_normalize(buf, "kenv hostname.override",
	    out, outsz))
		return (0);
	xlog("hostname from kenv hostname.override -> '%s'", out);
	return (1);
}

/* Tier 2: SCPreferences /System/System/ComputerName (user-set name via
 * UI / scutil — beats synthesis). Opens the default preferences.plist
 * (/Library/Preferences/SystemConfiguration/preferences.plist),
 * drills the /System/System dict, reads the ComputerName string.
 * Returns 1 if a valid name was found and stored in out; 0 otherwise.
 * On any SC error or missing/empty/invalid value, falls through to
 * synthesis — never aborts the daemon. Issue: #86 */
static int
try_scprefs(char *out, size_t outsz)
{
	SCPreferencesRef prefs = NULL;
	CFStringRef session = NULL, path = NULL, key = NULL;
	CFDictionaryRef sysdict;
	CFStringRef cf_name;
	char buf[256];
	int rc = 0;

	session = mkstr("com.apple.hostnamed");
	path = mkstr("/System/System");
	key = mkstr("ComputerName");
	if (session == NULL || path == NULL || key == NULL)
		goto out;

	/* SCPreferencesCreate is lazy — it doesn't fail just because the
	 * prefs file is missing. SCPreferencesPathGetValue will return
	 * NULL in that case, and we silently fall through. */
	prefs = SCPreferencesCreate(NULL, session, NULL);
	if (prefs == NULL) {
		xlog("try_scprefs: SCPreferencesCreate failed: %s "
		    "(falling through to synthesis)",
		    SCErrorString(SCError()));
		goto out;
	}
	sysdict = SCPreferencesPathGetValue(prefs, path);
	if (sysdict == NULL ||
	    CFGetTypeID(sysdict) != CFDictionaryGetTypeID())
		goto out;
	cf_name = CFDictionaryGetValue(sysdict, key);
	if (cf_name == NULL || CFGetTypeID(cf_name) != CFStringGetTypeID())
		goto out;
	if (!CFStringGetCString(cf_name, buf, sizeof(buf),
	    kCFStringEncodingUTF8))
		goto out;
	if (!validate_and_normalize(buf,
	    "SCPrefs /System/System/ComputerName", out, outsz))
		goto out;
	xlog("hostname from SCPrefs /System/System/ComputerName -> '%s'",
	    out);
	rc = 1;
out:
	if (prefs != NULL) CFRelease(prefs);
	if (key != NULL) CFRelease(key);
	if (path != NULL) CFRelease(path);
	if (session != NULL) CFRelease(session);
	return (rc);
}

/* Tier 3a: DHCP option 12 (host_name) — server-supplied name from the
 * active lease. ipconfigd (issue #88) publishes
 * State:/Network/Service/<UUID>/DHCP dicts; we enumerate via the regex
 * pattern, read each dict's Option_12 (CFString), validate the first
 * non-empty value via the shared whitelist, and use it. Returns 1 if
 * a usable value was found, 0 otherwise. Any SC error / missing key /
 * absent Option_12 / wrong type / rejected value falls through to
 * synthesis — never aborts the daemon. Issue: #90 */
static int
try_dhcp(char *out, size_t outsz)
{
	SCDynamicStoreRef store = NULL;
	CFStringRef session = NULL, pattern = NULL, k_opt12 = NULL;
	CFArrayRef keys = NULL;
	CFIndex i, n;
	int rc = 0;

	session = mkstr("com.apple.hostnamed");
	/* POSIX-regex over the SCDynamicStore key namespace. Matches
	 * any service UUID; iter 3a takes the first dict that carries a
	 * non-empty Option_12. Iter 3b will use State:/Network/Global/IPv4
	 * to identify the primary service and read only its /DHCP. */
	pattern = mkstr("State:/Network/Service/[^/]+/DHCP");
	k_opt12 = mkstr("Option_12");
	if (session == NULL || pattern == NULL || k_opt12 == NULL)
		goto out;

	store = SCDynamicStoreCreate(NULL, session, NULL, NULL);
	if (store == NULL) {
		xlog("try_dhcp: SCDynamicStoreCreate failed: %s "
		    "(falling through to synthesis)",
		    SCErrorString(SCError()));
		goto out;
	}

	keys = SCDynamicStoreCopyKeyList(store, pattern);
	if (keys == NULL)
		goto out;
	n = CFArrayGetCount(keys);
	for (i = 0; i < n; i++) {
		CFStringRef key;
		CFPropertyListRef plist;
		CFStringRef cf_name;
		char buf[256];

		key = (CFStringRef)CFArrayGetValueAtIndex(keys, i);
		if (key == NULL)
			continue;
		plist = SCDynamicStoreCopyValue(store, key);
		if (plist == NULL)
			continue;
		if (CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
			CFRelease(plist);
			continue;
		}
		cf_name = CFDictionaryGetValue((CFDictionaryRef)plist,
		    k_opt12);
		if (cf_name == NULL ||
		    CFGetTypeID(cf_name) != CFStringGetTypeID()) {
			CFRelease(plist);
			continue;
		}
		if (!CFStringGetCString(cf_name, buf, sizeof(buf),
		    kCFStringEncodingUTF8)) {
			CFRelease(plist);
			continue;
		}
		CFRelease(plist);
		if (!validate_and_normalize(buf,
		    "DHCP Option_12", out, outsz))
			continue;
		xlog("hostname from DHCP /Network/Service/.../DHCP/Option_12 "
		    "-> '%s'", out);
		rc = 1;
		break;
	}
out:
	if (keys != NULL) CFRelease(keys);
	if (store != NULL) CFRelease(store);
	if (k_opt12 != NULL) CFRelease(k_opt12);
	if (pattern != NULL) CFRelease(pattern);
	if (session != NULL) CFRelease(session);
	return (rc);
}

/* Tier 3b: mDNS PTR lookup. Find our bound IPv4 via
 * State:/Network/Service/<UUID>/IPv4 (Addresses array element 0), build
 * the reverse in-addr.arpa name, issue a PTR query via libdns_sd with
 * kDNSServiceFlagsForceMulticast so it hits mDNS not unicast DNS, and
 * extract the first label from the returned name. If a peer on the link
 * has registered an A record for our IP, the returned PTR tells us the
 * .local name they know us by — we adopt that first label as our
 * hostname. Returns 1 if a usable PTR answer was decoded; 0 on no IPv4
 * yet / timeout / decode failure / validation reject. Always falls
 * through to synthesis on failure — never aborts the daemon. */
#define MDNS_QUERY_TIMEOUT	5	/* seconds wall-clock budget */

struct mdns_ctx {
	char *out;
	size_t outsz;
	int got_answer;
	int success;
};

static void DNSSD_API
mdns_ptr_cb(DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t ifIdx,
    DNSServiceErrorType err, const char *fullname, uint16_t rrtype,
    uint16_t rrclass, uint16_t rdlen, const void *rdata, uint32_t ttl,
    void *context)
{
	struct mdns_ctx *ctx = (struct mdns_ctx *)context;
	const uint8_t *p;
	uint8_t label_len;
	char label[64];

	(void)sdRef;
	(void)flags;
	(void)ifIdx;
	(void)fullname;
	(void)rrtype;
	(void)rrclass;
	(void)ttl;

	ctx->got_answer = 1;
	if (err != kDNSServiceErr_NoError) {
		xlog("try_mdns: callback err=%d", (int)err);
		return;
	}
	if (rdata == NULL || rdlen < 2) {
		xlog("try_mdns: rdata empty (rdlen=%u)", (unsigned)rdlen);
		return;
	}
	p = (const uint8_t *)rdata;
	label_len = p[0];
	/* Reject DNS compression pointers (top two bits set, 0xC0+) — they
	 * shouldn't appear in libdns_sd callbacks (uds_daemon delivers
	 * decompressed labels) but defend anyway. Also reject 0-length and
	 * any length that would overrun rdata. */
	if (label_len == 0 || label_len >= 64 ||
	    (uint16_t)(1 + label_len) > rdlen) {
		xlog("try_mdns: rdata first label invalid "
		    "(label_len=%u rdlen=%u)",
		    (unsigned)label_len, (unsigned)rdlen);
		return;
	}
	(void)memcpy(label, p + 1, label_len);
	label[label_len] = '\0';
	if (!validate_and_normalize(label, "mDNS PTR",
	    ctx->out, ctx->outsz))
		return;
	ctx->success = 1;
}

/* Find a usable bound IPv4 in State:/Network/Service/<UUID>/IPv4 's
 * Addresses array (key + dict shape per src/IPConfiguration/sc_publish.c:
 * 199-220). Skips loopback, unspecified, and link-local. Returns 1 if a
 * dotted-quad was copied into out; 0 otherwise. iter 3b doesn't yet
 * consult State:/Network/Global/IPv4 to pick the primary service —
 * same as iter 3a's try_dhcp, deferred. */
static int
pick_primary_ipv4(SCDynamicStoreRef store, char *out, size_t outsz)
{
	CFStringRef pattern = NULL, k_addresses = NULL;
	CFArrayRef keys = NULL;
	CFIndex i, n;
	int rc = 0;

	pattern = mkstr("State:/Network/Service/[^/]+/IPv4");
	k_addresses = mkstr("Addresses");
	if (pattern == NULL || k_addresses == NULL)
		goto out;

	keys = SCDynamicStoreCopyKeyList(store, pattern);
	if (keys == NULL)
		goto out;
	n = CFArrayGetCount(keys);
	for (i = 0; i < n; i++) {
		CFStringRef key;
		CFPropertyListRef plist;
		CFArrayRef addrs;
		CFStringRef addr0;
		char buf[INET_ADDRSTRLEN];

		key = (CFStringRef)CFArrayGetValueAtIndex(keys, i);
		if (key == NULL)
			continue;
		plist = SCDynamicStoreCopyValue(store, key);
		if (plist == NULL)
			continue;
		if (CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
			CFRelease(plist);
			continue;
		}
		addrs = CFDictionaryGetValue((CFDictionaryRef)plist,
		    k_addresses);
		if (addrs == NULL ||
		    CFGetTypeID(addrs) != CFArrayGetTypeID() ||
		    CFArrayGetCount(addrs) == 0) {
			CFRelease(plist);
			continue;
		}
		addr0 = (CFStringRef)CFArrayGetValueAtIndex(addrs, 0);
		if (addr0 == NULL ||
		    CFGetTypeID(addr0) != CFStringGetTypeID()) {
			CFRelease(plist);
			continue;
		}
		if (!CFStringGetCString(addr0, buf, sizeof(buf),
		    kCFStringEncodingUTF8)) {
			CFRelease(plist);
			continue;
		}
		CFRelease(plist);
		if (strncmp(buf, "127.", 4) == 0 ||
		    strcmp(buf, "0.0.0.0") == 0 ||
		    strncmp(buf, "169.254.", 8) == 0)
			continue;
		(void)strncpy(out, buf, outsz - 1);
		out[outsz - 1] = '\0';
		rc = 1;
		break;
	}
out:
	if (keys != NULL) CFRelease(keys);
	if (k_addresses != NULL) CFRelease(k_addresses);
	if (pattern != NULL) CFRelease(pattern);
	return (rc);
}

/* Build reverse in-addr.arpa name from a dotted-quad IPv4 string.
 * "10.0.2.15" -> "15.2.0.10.in-addr.arpa". */
static int
build_reverse_inaddr(const char *ipv4, char *out, size_t outsz)
{
	unsigned a, b, c, d;
	int n;

	if (sscanf(ipv4, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
		return (0);
	if (a > 255 || b > 255 || c > 255 || d > 255)
		return (0);
	n = snprintf(out, outsz, "%u.%u.%u.%u.in-addr.arpa",
	    d, c, b, a);
	return (n > 0 && (size_t)n < outsz);
}

static int
try_mdns(char *out, size_t outsz)
{
	SCDynamicStoreRef store = NULL;
	CFStringRef session = NULL;
	DNSServiceRef sd_ref = NULL;
	DNSServiceErrorType derr;
	struct mdns_ctx ctx;
	char ipv4[INET_ADDRSTRLEN];
	char reverse[128];
	int sock_fd;
	time_t deadline;
	int rc = 0;

	memset(&ctx, 0, sizeof(ctx));
	ctx.out = out;
	ctx.outsz = outsz;

	session = mkstr("com.apple.hostnamed");
	if (session == NULL)
		goto out;
	store = SCDynamicStoreCreate(NULL, session, NULL, NULL);
	if (store == NULL) {
		xlog("try_mdns: SCDynamicStoreCreate failed: %s "
		    "(falling through to synthesis)",
		    SCErrorString(SCError()));
		goto out;
	}
	if (!pick_primary_ipv4(store, ipv4, sizeof(ipv4))) {
		xlog("try_mdns: no usable IPv4 in "
		    "State:/Network/Service/<UUID>/IPv4 yet");
		goto out;
	}
	if (!build_reverse_inaddr(ipv4, reverse, sizeof(reverse))) {
		xlog("try_mdns: build_reverse_inaddr failed for '%s'", ipv4);
		goto out;
	}
	xlog("try_mdns: PTR query %s (primary IPv4 %s)", reverse, ipv4);

	derr = DNSServiceQueryRecord(&sd_ref,
	    kDNSServiceFlagsForceMulticast | kDNSServiceFlagsTimeout,
	    0 /* any interface */, reverse,
	    kDNSServiceType_PTR, kDNSServiceClass_IN,
	    mdns_ptr_cb, &ctx);
	if (derr != kDNSServiceErr_NoError) {
		xlog("try_mdns: DNSServiceQueryRecord returned %d",
		    (int)derr);
		goto out;
	}
	sock_fd = DNSServiceRefSockFD(sd_ref);
	if (sock_fd < 0) {
		xlog("try_mdns: DNSServiceRefSockFD returned %d", sock_fd);
		goto out;
	}

	/* Drive the libdns_sd socket through select() with a 5s wall-clock
	 * deadline (mirrors dnssdtest.c). kDNSServiceFlagsTimeout drives a
	 * system-side timeout independently — whichever fires first ends
	 * the wait by firing the callback with err=Timeout. */
	deadline = time(NULL) + MDNS_QUERY_TIMEOUT;
	while (!ctx.got_answer && time(NULL) < deadline) {
		fd_set rfds;
		struct timeval tv;
		int r;

		FD_ZERO(&rfds);
		FD_SET(sock_fd, &rfds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		r = select(sock_fd + 1, &rfds, NULL, NULL, &tv);
		if (r > 0 && FD_ISSET(sock_fd, &rfds))
			(void)DNSServiceProcessResult(sd_ref);
	}
	if (!ctx.got_answer) {
		xlog("try_mdns: PTR %s timed out after %ds",
		    reverse, MDNS_QUERY_TIMEOUT);
		goto out;
	}
	if (ctx.success) {
		xlog("hostname from mDNS PTR %s -> '%s'", reverse, out);
		rc = 1;
	}
out:
	if (sd_ref != NULL) DNSServiceRefDeallocate(sd_ref);
	if (store != NULL) CFRelease(store);
	if (session != NULL) CFRelease(session);
	return (rc);
}

/* Compose the final hostname into out (HOSTNAMED_MAX chars). */
static void
synthesize(char *out, size_t outsz)
{
	char slug[SLUG_MAX + 1];
	char suffix[SUFFIX_LEN + 1];

	if (try_override(out, outsz))
		return;
	if (try_scprefs(out, outsz))
		return;
	if (try_dhcp(out, outsz))
		return;
	if (try_mdns(out, outsz))
		return;

	derive_slug(slug, sizeof(slug));
	if (derive_suffix(suffix) == 0) {
		(void)snprintf(out, outsz, "%s-%s", slug, suffix);
	} else {
		/* T3 final fallback. Should not happen in practice
		 * (kern.hostuuid is always non-empty). */
		(void)strncpy(out, "freebsd", outsz - 1);
		out[outsz - 1] = '\0';
		xlog("WARN: suffix derivation failed all tiers, "
		    "using bare 'freebsd'");
	}
	/* Truncate to 63 chars per RFC 1035 label limit. */
	if (strlen(out) > 63)
		out[63] = '\0';
	xlog("synthesized hostname -> '%s'", out);
}

/* Build the Setup:/System dict and SetValue it. Plan §4 Key 1.
 * Apple's ComputerName is a free-form UTF-8 string ("My Mac") with the
 * encoding stored alongside; we always use UTF-8. */
static int
publish_system(SCDynamicStoreRef store, const char *name)
{
	CFStringRef key = NULL, k_name = NULL, k_enc = NULL;
	CFStringRef v_name = NULL;
	CFNumberRef v_enc = NULL;
	CFMutableDictionaryRef dict = NULL;
	int32_t enc = (int32_t)kCFStringEncodingUTF8;
	int rc = -1;

	key = mkstr(SC_KEY_SYSTEM);
	k_name = mkstr("ComputerName");
	k_enc = mkstr("ComputerNameEncoding");
	v_name = mkstr(name);
	v_enc = CFNumberCreate(NULL, kCFNumberSInt32Type, &enc);
	dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	if (key == NULL || k_name == NULL || k_enc == NULL ||
	    v_name == NULL || v_enc == NULL || dict == NULL) {
		xlog("publish_system: CF allocation failed");
		goto out;
	}
	CFDictionarySetValue(dict, k_name, v_name);
	CFDictionarySetValue(dict, k_enc, v_enc);

	if (!SCDynamicStoreSetValue(store, key, dict)) {
		xlog("SCDynamicStoreSetValue(%s) failed: %s",
		    SC_KEY_SYSTEM, SCErrorString(SCError()));
		goto out;
	}
	rc = 0;
out:
	if (dict != NULL) CFRelease(dict);
	if (v_enc != NULL) CFRelease(v_enc);
	if (v_name != NULL) CFRelease(v_name);
	if (k_enc != NULL) CFRelease(k_enc);
	if (k_name != NULL) CFRelease(k_name);
	if (key != NULL) CFRelease(key);
	return (rc);
}

/* Build the Setup:/Network/HostNames dict and SetValue it. Plan §4 Key 2.
 * HostName (DNS-safe form) + LocalHostName (Bonjour name); iter 1 uses
 * the same synthesized value for both. iter 2 will diverge LocalHostName
 * when Bonjour conflict feedback lands. */
static int
publish_hostnames(SCDynamicStoreRef store, const char *name)
{
	CFStringRef key = NULL, k_host = NULL, k_local = NULL;
	CFStringRef v_name = NULL;
	CFMutableDictionaryRef dict = NULL;
	int rc = -1;

	key = mkstr(SC_KEY_HOSTNAMES);
	k_host = mkstr("HostName");
	k_local = mkstr("LocalHostName");
	v_name = mkstr(name);
	dict = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	if (key == NULL || k_host == NULL || k_local == NULL ||
	    v_name == NULL || dict == NULL) {
		xlog("publish_hostnames: CF allocation failed");
		goto out;
	}
	CFDictionarySetValue(dict, k_host, v_name);
	CFDictionarySetValue(dict, k_local, v_name);

	if (!SCDynamicStoreSetValue(store, key, dict)) {
		xlog("SCDynamicStoreSetValue(%s) failed: %s",
		    SC_KEY_HOSTNAMES, SCErrorString(SCError()));
		goto out;
	}
	rc = 0;
out:
	if (dict != NULL) CFRelease(dict);
	if (v_name != NULL) CFRelease(v_name);
	if (k_local != NULL) CFRelease(k_local);
	if (k_host != NULL) CFRelease(k_host);
	if (key != NULL) CFRelease(key);
	return (rc);
}

/* refresh_hostname — re-walk the precedence chain, sethostname, publish
 * to SCDynamicStore, broadcast notify_post. Idempotent: memoizes the
 * last-published value in a function-static buffer and skips all output
 * (no sethostname, no SCDS Set, no notify_post) when the synthesized
 * value is identical to last time. Called once at startup (boot-time
 * synthesis) and again on every SCDS/SCPrefs notification + SIGHUP.
 *
 * The trigger label distinguishes reasons in the log — "boot", "SCDS",
 * "SCPrefs", "SIGHUP" — so failures are diagnosable. */
static void
refresh_hostname(SCDynamicStoreRef store, const char *trigger)
{
	static char last_published[HOSTNAMED_MAX];
	char name[HOSTNAMED_MAX];

	synthesize(name, sizeof(name));

	if (strcmp(name, last_published) == 0) {
		xlog("refresh_hostname[%s]: unchanged ('%s')", trigger, name);
		return;
	}

	xlog("refresh_hostname[%s]: '%s' -> '%s'",
	    trigger, last_published[0] ? last_published : "(none)", name);

	/* sethostname(2) FIRST — preserves PAM iter 4's banner-race
	 * guarantee on the boot-time refresh path. */
	if (sethostname(name, (int)strlen(name)) != 0) {
		xlog("HOSTNAMED-FAIL: sethostname: %s", strerror(errno));
		return;
	}

	if (publish_system(store, name) != 0)
		xlog("HOSTNAMED-FAIL: publish_system");
	if (publish_hostnames(store, name) != 0)
		xlog("HOSTNAMED-FAIL: publish_hostnames");

	/* notify_post("com.apple.system.hostname") is intentionally
	 * SKIPPED in iter 3c. CI bisect xlogs (commit 505d016) pinned
	 * the iter-3c restart loop on notify_post: every refresh
	 * reached post-publish_hostnames then died silently — no
	 * post-notify_post log, KeepAlive=true relaunched at launchd's
	 * 10s throttle interval. The iter-3a/3b one-shot daemons called
	 * notify_post successfully and exited; only the iter-3c
	 * persistent shape provokes the crash, suggesting libnotify's
	 * lazy state interacts badly with the upcoming libdispatch /
	 * dispatch_main setup. The notify was already marked non-fatal
	 * (the WARN-on-non-OK path), and mDNSResponder + the SC
	 * publishes still announce the change, so dropping it
	 * temporarily is a safe degradation. See follow-up issue for
	 * the proper fix: defer the notify_post to a dispatch_async on
	 * the events queue so it runs in steady-state context, AFTER
	 * dispatch_main has bootstrapped libdispatch. */

	(void)strncpy(last_published, name, sizeof(last_published) - 1);
	last_published[sizeof(last_published) - 1] = '\0';

	xlog("HOSTNAMED-OK: published '%s' "
	    "(Setup:/System + Setup:/Network/HostNames + sethostname; "
	    "notify_post skipped for iter 3c)",
	    name);
}

/* Two-store split, mirroring scnotifytest.c's writer/watcher pattern:
 * we use one SCDynamicStoreRef without a callback for SCDynamicStoreSetValue
 * (the "publish" path used by publish_system / publish_hostnames), and a
 * separate one with a callback for SCDynamicStoreSetNotificationKeys +
 * SCDynamicStoreSetDispatchQueue (the "watch" path). This avoids the
 * SC-internal interaction where publishing on a callback-bearing store
 * before its dispatch queue is attached crashes the daemon (observed
 * silent death in the initial iter-3c CI run).
 *
 * Callbacks need access to the PUBLISH store (so refresh_hostname can
 * SCDynamicStoreSetValue), which we pass via the dispatch source /
 * SCPreferences info pointer. */
struct hostnamed_ctx {
	SCDynamicStoreRef publish_store;
};

/* SCDynamicStore notification callback. Fires when any watched key
 * changes (DHCP Option_12 mutates, IPv4 address rebinds, etc.).
 *
 * iter 3c bring-up: log the changed keys so we can identify whatever
 * is churning at 5s intervals and driving a runaway refresh loop in
 * CI. Each refresh_hostname includes a 5s try_mdns wait; if any
 * watched key updates that often, the daemon burns its event-loop
 * budget on no-op refreshes and can't react to the test rounds'
 * actual mutations in time. */
static void
scds_cb(SCDynamicStoreRef store, CFArrayRef changedKeys, void *info)
{
	struct hostnamed_ctx *ctx = (struct hostnamed_ctx *)info;
	CFIndex i, n;

	(void)store;
	if (ctx == NULL || ctx->publish_store == NULL)
		return;
	if (changedKeys != NULL) {
		n = CFArrayGetCount(changedKeys);
		for (i = 0; i < n; i++) {
			CFStringRef k = (CFStringRef)CFArrayGetValueAtIndex(
			    changedKeys, i);
			char buf[256];
			if (k != NULL && CFStringGetCString(k, buf,
			    sizeof(buf), kCFStringEncodingUTF8))
				xlog("scds_cb: changedKey[%ld]=%s",
				    (long)i, buf);
		}
	}
	refresh_hostname(ctx->publish_store, "SCDS");
}

/* SCPreferences commit callback. Fires when /Library/Preferences/
 * SystemConfiguration/preferences.plist is committed — covers the iter
 * 2 ComputerName tier (hostnameprefset is the CI fixture that
 * exercises this path). */
static void
scprefs_cb(SCPreferencesRef prefs, SCPreferencesNotification notificationType,
    void *info)
{
	struct hostnamed_ctx *ctx = (struct hostnamed_ctx *)info;

	(void)prefs;
	(void)notificationType;
	if (ctx == NULL || ctx->publish_store == NULL)
		return;
	refresh_hostname(ctx->publish_store, "SCPrefs");
}

/* dispatch_source_set_event_handler_f context-bearing callbacks. The
 * SCDynamicStoreRef is set into the source's context via
 * dispatch_set_context. SIGHUP triggers a re-synth (covers the mDNS
 * round in CI, since mDNS PTR appearance isn't an SCDS event, and
 * any operator/script nudge). SIGTERM/SIGINT cleanly exit
 * dispatch_main(). */
static void
sighup_handler(void *ctx)
{
	refresh_hostname((SCDynamicStoreRef)ctx, "SIGHUP");
}

static void
sigterm_handler(void *ctx)
{
	(void)ctx;
	xlog("SIGTERM/INT — exiting");
	exit(0);
}

/* Build the watched-keys + watched-patterns CFArrays for
 * SCDynamicStoreSetNotificationKeys. Patterns cover the iter 3a
 * (DHCP Option_12) and iter 3b (IPv4 → mDNS PTR) tiers. The explicit
 * Global/IPv4 key prepares for primary-service selection (deferred —
 * try_dhcp/try_mdns don't consume it yet, but registering now is
 * harmless and avoids a churn iter later). */
static int
setup_scds_notifications(SCDynamicStoreRef store)
{
	CFStringRef pat_dhcp = NULL, pat_ipv4 = NULL, k_global = NULL;
	CFArrayRef patterns = NULL, keys = NULL;
	const void *pat_arr[2];
	const void *key_arr[1];
	int rc = -1;

	pat_dhcp = mkstr("State:/Network/Service/.+/DHCP");
	pat_ipv4 = mkstr("State:/Network/Service/.+/IPv4");
	k_global = mkstr("State:/Network/Global/IPv4");
	if (pat_dhcp == NULL || pat_ipv4 == NULL || k_global == NULL)
		goto out;

	pat_arr[0] = pat_dhcp;
	pat_arr[1] = pat_ipv4;
	patterns = CFArrayCreate(NULL, pat_arr, 2, &kCFTypeArrayCallBacks);

	key_arr[0] = k_global;
	keys = CFArrayCreate(NULL, key_arr, 1, &kCFTypeArrayCallBacks);
	if (patterns == NULL || keys == NULL)
		goto out;

	if (!SCDynamicStoreSetNotificationKeys(store, keys, patterns)) {
		xlog("HOSTNAMED-FAIL: SCDynamicStoreSetNotificationKeys: %s",
		    SCErrorString(SCError()));
		goto out;
	}
	rc = 0;
out:
	if (keys != NULL) CFRelease(keys);
	if (patterns != NULL) CFRelease(patterns);
	if (k_global != NULL) CFRelease(k_global);
	if (pat_ipv4 != NULL) CFRelease(pat_ipv4);
	if (pat_dhcp != NULL) CFRelease(pat_dhcp);
	return (rc);
}

int
main(int argc, char **argv)
{
	SCDynamicStoreRef publish_store = NULL;
	SCDynamicStoreRef watch_store = NULL;
	SCPreferencesRef prefs = NULL;
	SCDynamicStoreContext scds_ctx;
	static struct hostnamed_ctx ctx;
	dispatch_queue_t queue;
	dispatch_source_t sig_hup_src, sig_term_src, sig_int_src;
	CFStringRef publish_name = NULL, watch_name = NULL, prefs_id = NULL;
	sigset_t mask;

	(void)argc;
	(void)argv;

	xlog("starting (iter 3c: persistent libdispatch event loop)");

	publish_name = mkstr("com.apple.hostnamed.publish");
	watch_name = mkstr("com.apple.hostnamed.watch");
	prefs_id = mkstr("preferences.plist");
	if (publish_name == NULL || watch_name == NULL || prefs_id == NULL) {
		xlog("HOSTNAMED-FAIL: CFStringCreate failed");
		return (1);
	}

	/* Publish store has NO callback — used only for SCDynamicStoreSetValue
	 * in publish_system / publish_hostnames. Mirrors scnotifytest.c's
	 * "writer" half of the pattern. */
	publish_store = SCDynamicStoreCreate(NULL, publish_name, NULL, NULL);
	if (publish_store == NULL) {
		xlog("HOSTNAMED-FAIL: SCDynamicStoreCreate(publish): %s",
		    SCErrorString(SCError()));
		return (1);
	}

	/* Initial boot-time refresh. Uses publish_store only; PAM iter 4's
	 * banner-race guarantee preserved — sethostname runs before any of
	 * the dispatch / SCDS-notify / SCPrefs / signal-source setup work
	 * below, so it still beats getty (when the race is winnable). */
	refresh_hostname(publish_store, "boot");

	/* Now stand up the watch path. From here on the daemon is reactive
	 * — every SCDS / SCPrefs / SIGHUP triggers refresh_hostname via
	 * the publish_store carried in `ctx`. */
	queue = dispatch_queue_create("com.apple.hostnamed.events", NULL);
	if (queue == NULL) {
		xlog("HOSTNAMED-FAIL: dispatch_queue_create");
		return (1);
	}

	ctx.publish_store = publish_store;

	memset(&scds_ctx, 0, sizeof(scds_ctx));
	scds_ctx.info = &ctx;
	watch_store = SCDynamicStoreCreate(NULL, watch_name, scds_cb,
	    &scds_ctx);
	if (watch_store == NULL) {
		xlog("HOSTNAMED-FAIL: SCDynamicStoreCreate(watch): %s",
		    SCErrorString(SCError()));
		return (1);
	}

	if (setup_scds_notifications(watch_store) != 0)
		return (1);
	if (!SCDynamicStoreSetDispatchQueue(watch_store, queue)) {
		xlog("HOSTNAMED-FAIL: SCDynamicStoreSetDispatchQueue: %s",
		    SCErrorString(SCError()));
		return (1);
	}
	xlog("SCDS notifications: scheduled on dispatch queue "
	    "(patterns: /DHCP, /IPv4; key: Global/IPv4)");

	prefs = SCPreferencesCreate(NULL, watch_name, prefs_id);
	if (prefs == NULL) {
		xlog("HOSTNAMED-FAIL: SCPreferencesCreate: %s",
		    SCErrorString(SCError()));
		return (1);
	}
	{
		SCPreferencesContext pctx;
		memset(&pctx, 0, sizeof(pctx));
		pctx.info = &ctx;
		if (!SCPreferencesSetCallback(prefs, scprefs_cb, &pctx)) {
			xlog("HOSTNAMED-FAIL: SCPreferencesSetCallback: %s",
			    SCErrorString(SCError()));
			return (1);
		}
	}
	if (!SCPreferencesSetDispatchQueue(prefs, queue)) {
		xlog("HOSTNAMED-FAIL: SCPreferencesSetDispatchQueue: %s",
		    SCErrorString(SCError()));
		return (1);
	}
	xlog("SCPrefs notifications: scheduled on dispatch queue "
	    "(/System/System/ComputerName watch)");

	/* Block SIGHUP/TERM/INT in the C signal-mask sense, then create
	 * dispatch sources to deliver them via EVFILT_SIGNAL on our queue.
	 * Without the block, the kernel would default-kill the process on
	 * the first SIGHUP. Pattern from notifyd.c:1480-1503. */
	(void)sigemptyset(&mask);
	(void)sigaddset(&mask, SIGHUP);
	(void)sigaddset(&mask, SIGTERM);
	(void)sigaddset(&mask, SIGINT);
	(void)sigprocmask(SIG_BLOCK, &mask, NULL);

	sig_hup_src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL,
	    (uintptr_t)SIGHUP, 0, queue);
	dispatch_set_context(sig_hup_src, publish_store);
	dispatch_source_set_event_handler_f(sig_hup_src, sighup_handler);
	dispatch_activate(sig_hup_src);

	sig_term_src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL,
	    (uintptr_t)SIGTERM, 0, queue);
	dispatch_source_set_event_handler_f(sig_term_src, sigterm_handler);
	dispatch_activate(sig_term_src);

	sig_int_src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL,
	    (uintptr_t)SIGINT, 0, queue);
	dispatch_source_set_event_handler_f(sig_int_src, sigterm_handler);
	dispatch_activate(sig_int_src);

	xlog("event loop entered — waiting for SCDS/SCPrefs deltas and signals");
	dispatch_main();
	/* NOTREACHED */
}
