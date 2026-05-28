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

int
main(int argc, char **argv)
{
	char name[HOSTNAMED_MAX];
	SCDynamicStoreRef store = NULL;
	CFStringRef session = NULL;
	uint32_t nrc;
	int rc = 1;

	(void)argc;
	(void)argv;

	xlog("starting (iter 1: synthesize + publish hostname, issue #63)");

	synthesize(name, sizeof(name));

	/*
	 * sethostname(2) FIRST — moved here at PAM port iter 4 (#99) so
	 * the kernel hostname is set as early as possible after fork+exec,
	 * winning the race against getty's banner-print at boot. The
	 * SCDynamicStore publishes below do ~50-80ms of CF allocation
	 * work; without this reorder, getty (dispatched earlier in the
	 * launchd plist scan) finishes its banner before sethostname()
	 * gets called.
	 *
	 * sethostname() needs nothing CF-side, just the synthesized
	 * string. Failure here is fatal (we can't continue with
	 * publish_*) but extremely unlikely in practice.
	 */
	if (sethostname(name, (int)strlen(name)) != 0) {
		xlog("HOSTNAMED-FAIL: sethostname: %s", strerror(errno));
		goto out;
	}

	session = mkstr("com.apple.hostnamed");
	if (session == NULL) {
		xlog("HOSTNAMED-FAIL: CFStringCreate failed for session name");
		goto out;
	}
	store = SCDynamicStoreCreate(NULL, session, NULL, NULL);
	if (store == NULL) {
		xlog("HOSTNAMED-FAIL: SCDynamicStoreCreate: %s",
		    SCErrorString(SCError()));
		goto out;
	}

	if (publish_system(store, name) != 0) {
		xlog("HOSTNAMED-FAIL: publish_system");
		goto out;
	}
	if (publish_hostnames(store, name) != 0) {
		xlog("HOSTNAMED-FAIL: publish_hostnames");
		goto out;
	}

	/* notify_post is the Apple-canonical broadcast on hostname change.
	 * mDNSResponder, the ASL store, and any other notify_register_check
	 * client picks up the new value on this token. */
	nrc = notify_post("com.apple.system.hostname");
	if (nrc != NOTIFY_STATUS_OK) {
		/* Non-fatal: notifyd may not be up yet at first boot, but
		 * the store + kernel hostname are already set. */
		xlog("WARN: notify_post returned %u (non-fatal)",
		    (unsigned)nrc);
	}

	xlog("HOSTNAMED-OK: published '%s' "
	    "(Setup:/System + Setup:/Network/HostNames + sethostname)",
	    name);
	rc = 0;
out:
	if (store != NULL) CFRelease(store);
	if (session != NULL) CFRelease(session);
	return (rc);
}
