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

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>

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

	if (sethostname(name, (int)strlen(name)) != 0) {
		xlog("HOSTNAMED-FAIL: sethostname: %s", strerror(errno));
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
