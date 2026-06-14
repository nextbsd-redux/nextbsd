/*
 * freebsd-launchd-mach hostnamed freebsd-shim — implementations.
 *
 * Bundles four small categories of code that the vendored Apple
 * set-hostname.c needs but our environment doesn't supply:
 *
 *   1. ip_plugin.h surface — copy_dhcp_hostname (SCDS reader),
 *      check_if_service_expensive (always FALSE),
 *      hostnamed_xlog_cf (the my_log macro target).
 *   2. SCPrivate.h SPI subset — _SC_string_to_sockaddr,
 *      _SC_cfstring_to_cstring, _SC_CFStringIsValidDNSName.
 *   3. freebsd_synthesize_hostname — slug synthesis (SMBIOS model
 *      slug, e.g. "ThinkPad-T460s") for the "localhost" fallback
 *      substitution. No serial/MAC suffix is appended.
 *   4. The synthesis helper machinery itself (derive_slug,
 *      sanitize_slug), extracted from hostnamed.c — the new minimal
 *      hostnamed.c calls these for its boot-time prefs_monitor initial
 *      value, and the shim's freebsd_synthesize_hostname wraps them too.
 *
 * Everything is in one file so the build picks up a small additional
 * SRCS surface; the individual responsibilities are demarcated by
 * #pragma mark sections below.
 */

#include "ip_plugin.h"
#include "SCPrivate.h"
#include "SCValidation.h"

#include <SystemConfiguration/SCDynamicStore.h>
#include <SystemConfiguration/SCSchemaDefinitions.h>

#include <CoreFoundation/CoreFoundation.h>

#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <ctype.h>
#include <ifaddrs.h>
#include <kenv.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#define HOSTNAMED_MAX	64
#define SLUG_MAX	40

/* hostnamed's plain-C log surface (definition in hostnamed.c). */
extern void	xlog(const char *fmt, ...);

#pragma mark -
#pragma mark my_log → xlog routing

void
hostnamed_xlog_cf(int level, CFStringRef format, ...)
{
	CFStringRef formatted = NULL;
	char buf[1024];
	va_list ap;

	(void)level;	/* drop syslog severity for now */
	if (format == NULL)
		return;
	va_start(ap, format);
	formatted = CFStringCreateWithFormatAndArguments(NULL, NULL, format,
	    ap);
	va_end(ap);
	if (formatted == NULL)
		return;
	if (CFStringGetCString(formatted, buf, sizeof(buf),
	    kCFStringEncodingUTF8))
		xlog("%s", buf);
	CFRelease(formatted);
}

#pragma mark -
#pragma mark synthesis machinery (slug from SMBIOS)

static int
sh_read_kenv(const char *name, char *out, size_t outsz)
{
	int n;

	if (outsz == 0 || outsz > INT_MAX)
		return (-1);
	n = kenv(KENV_GET, name, out, (int)outsz);
	if (n <= 0)
		return (-1);
	out[outsz - 1] = '\0';
	return (n);
}

static size_t
sh_sanitize_slug(char *s)
{
	size_t i, j;
	int prev_dash;

	if (s == NULL)
		return (0);
	j = 0;
	prev_dash = 1;
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
	while (j > 0 && s[j - 1] == '-')
		j--;
	s[j] = '\0';
	return (j);
}

/*
 * An SMBIOS field is "identifying" only if it carries at least one
 * letter. Real OEMs put the model name in smbios.system.version
 * (e.g. "ThinkPad T460s"), but synthetic firmware such as VirtualBox
 * stuffs the bare SMBIOS table revision there ("1.2"), which sanitizes
 * to the useless slug "1-2". A model name always contains an alpha
 * character; a dotted revision never does — so a single isalpha scan
 * cleanly separates the two without hard-coding vendor strings.
 */
static int
sh_value_is_identifying(const char *raw)
{
	size_t i;

	if (raw == NULL)
		return (0);
	for (i = 0; raw[i] != '\0'; i++) {
		if (isalpha((unsigned char)raw[i]))
			return (1);
	}
	return (0);
}

static int
sh_try_kenv_slug(const char *key, char *out, size_t outsz)
{
	char buf[256];

	if (sh_read_kenv(key, buf, sizeof(buf)) <= 0)
		return (0);
	if (!sh_value_is_identifying(buf))
		return (0);
	(void)strncpy(out, buf, outsz - 1);
	out[outsz - 1] = '\0';
	(void)sh_sanitize_slug(out);
	return (out[0] != '\0');
}

/*
 * Derive the host slug. Returns 1 when the slug is an identifying per-model
 * name (from smbios.system.version, e.g. "ThinkPad-T460s") that is unique
 * enough to use bare; returns 0 when it is a GENERIC fallback
 * (smbios.system.product such as "VirtualBox", shared by every guest of
 * that hypervisor, or the "freebsd" last resort) that the caller should
 * make unique with a per-machine suffix.
 */
static int
sh_derive_slug(char *out, size_t outsz)
{
	/*
	 * Prefer the model name in smbios.system.version, but skip it when
	 * it is a non-identifying revision (e.g. VirtualBox's "1.2") and
	 * fall through to smbios.system.product ("VirtualBox"). "freebsd"
	 * is the last resort.
	 */
	if (sh_try_kenv_slug("smbios.system.version", out, outsz))
		return (1);	/* real model name → unique enough, use bare */
	if (sh_try_kenv_slug("smbios.system.product", out, outsz))
		return (0);	/* generic product (e.g. VirtualBox) → add suffix */
	(void)strncpy(out, "freebsd", outsz - 1);
	out[outsz - 1] = '\0';
	return (0);		/* last resort → add suffix */
}

/*
 * Derive a short, stable, per-machine uniqueness suffix from the SMBIOS
 * system UUID (e.g. "68f9a871-8e8b-..." -> "68f9a871"). Two VMs of the same
 * hypervisor share smbios.system.product ("VirtualBox") but each gets a
 * distinct UUID, so this makes their synthesized hostnames unique at boot
 * without relying on Bonjour conflict-rename. Writes lowercase hex,
 * NUL-terminated; returns its length, or 0 if no usable (present, non-zero)
 * UUID is available.
 */
static size_t
sh_derive_uuid_suffix(char *out, size_t outsz)
{
	char buf[256];
	size_t i, j;

	if (outsz == 0)
		return (0);
	out[0] = '\0';
	if (sh_read_kenv("smbios.system.uuid", buf, sizeof(buf)) <= 0)
		return (0);
	/* Take up to the 8 leading hex digits (the UUID's first group). */
	j = 0;
	for (i = 0; buf[i] != '\0' && buf[i] != '-' && j < 8 && j < outsz - 1;
	    i++) {
		unsigned char c = (unsigned char)buf[i];
		if (isxdigit(c))
			out[j++] = (char)tolower(c);
	}
	out[j] = '\0';
	/* Reject an all-zero UUID (some firmware reports 00000000-...). */
	if (j > 0 && strspn(out, "0") == j)
		out[0] = '\0';
	return (strlen(out));
}

/* Public synthesis entry — used by set-hostname.c's localhost carry
 * AND by hostnamed's prefs_monitor (Commit 7) for the boot-time
 * fallback value when SCPrefs ComputerName is absent. */
CFStringRef
freebsd_synthesize_hostname(void)
{
	char slug[SLUG_MAX + 1];
	char name[HOSTNAMED_MAX];
	int identifying;

	/*
	 * Identifying model names (e.g. "ThinkPad-T460s" from
	 * smbios.system.version) are used bare. Generic fallbacks
	 * (smbios.system.product like "VirtualBox", or "freebsd") get a short
	 * per-machine suffix from the SMBIOS UUID appended — otherwise every
	 * guest of the same hypervisor would synthesize the identical hostname
	 * (e.g. all VirtualBox guests -> "VirtualBox", or "1-2" before the
	 * identifying-version fix). See sh_derive_slug / sh_derive_uuid_suffix.
	 */
	identifying = sh_derive_slug(slug, sizeof(slug));
	(void)strncpy(name, slug, sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';
	if (!identifying) {
		char suffix[16];

		if (sh_derive_uuid_suffix(suffix, sizeof(suffix)) > 0) {
			size_t len = strlen(name);

			(void)snprintf(name + len, sizeof(name) - len, "-%s",
			    suffix);
		}
	}
	if (strlen(name) > 63)
		name[63] = '\0';
	xlog("freebsd_synthesize_hostname -> '%s'", name);
	return (CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8));
}

#pragma mark -
#pragma mark ip_plugin.h externs

CFStringRef
copy_dhcp_hostname(CFStringRef serviceID)
{
	SCDynamicStoreRef store;
	CFStringRef key, opt12 = NULL;
	CFDictionaryRef dict;

	if (serviceID == NULL)
		return (NULL);
	store = SCDynamicStoreCreate(NULL, CFSTR("copy_dhcp_hostname"),
	    NULL, NULL);
	if (store == NULL)
		return (NULL);
	key = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
	    kSCDynamicStoreDomainState, serviceID, kSCEntNetDHCP);
	if (key == NULL) {
		CFRelease(store);
		return (NULL);
	}
	dict = (CFDictionaryRef)SCDynamicStoreCopyValue(store, key);
	CFRelease(key);
	CFRelease(store);
	if (dict == NULL)
		return (NULL);
	if (CFGetTypeID(dict) == CFDictionaryGetTypeID()) {
		CFStringRef v = CFDictionaryGetValue(dict, CFSTR("Option_12"));
		if (v != NULL && CFGetTypeID(v) == CFStringGetTypeID())
			opt12 = CFStringCreateCopy(NULL, v);
	}
	CFRelease(dict);
	return (opt12);
}

Boolean
check_if_service_expensive(CFStringRef serviceID)
{
	(void)serviceID;	/* no metered-network concept on FreeBSD */
	return (FALSE);
}

#pragma mark -
#pragma mark SCPrivate SPI subset

void *
_SC_string_to_sockaddr(const char *str, sa_family_t family, void *buf,
    size_t bufsize)
{
	struct sockaddr_in *sin = (struct sockaddr_in *)buf;
	struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)buf;

	if (str == NULL || buf == NULL)
		return (NULL);

	if (family == AF_INET || family == AF_UNSPEC) {
		if (bufsize < sizeof(*sin))
			return (NULL);
		memset(buf, 0, sizeof(*sin));
		sin->sin_family = AF_INET;
		sin->sin_len = sizeof(*sin);
		if (inet_pton(AF_INET, str, &sin->sin_addr) == 1)
			return (buf);
		if (family == AF_INET)
			return (NULL);
	}
	if (family == AF_INET6 || family == AF_UNSPEC) {
		if (bufsize < sizeof(*sin6))
			return (NULL);
		memset(buf, 0, sizeof(*sin6));
		sin6->sin6_family = AF_INET6;
		sin6->sin6_len = sizeof(*sin6);
		if (inet_pton(AF_INET6, str, &sin6->sin6_addr) == 1)
			return (buf);
	}
	return (NULL);
}

char *
_SC_cfstring_to_cstring(CFStringRef cfstr, char *buf, CFIndex bufsize,
    CFStringEncoding encoding)
{
	CFIndex maxsize;

	if (cfstr == NULL)
		return (NULL);
	if (buf != NULL) {
		if (CFStringGetCString(cfstr, buf, bufsize, encoding))
			return (buf);
		return (NULL);
	}
	/* allocated form */
	maxsize = CFStringGetMaximumSizeForEncoding(
	    CFStringGetLength(cfstr), encoding) + 1;
	buf = (char *)malloc((size_t)maxsize);
	if (buf == NULL)
		return (NULL);
	if (CFStringGetCString(cfstr, buf, maxsize, encoding))
		return (buf);
	free(buf);
	return (NULL);
}

Boolean
_SC_CFStringIsValidDNSName(CFStringRef cfstr)
{
	char buf[256];
	size_t i, len, label_len = 0;
	int label_count = 0;

	if (cfstr == NULL)
		return (FALSE);
	if (!CFStringGetCString(cfstr, buf, sizeof(buf), kCFStringEncodingUTF8))
		return (FALSE);
	len = strlen(buf);
	if (len == 0 || len > 253)
		return (FALSE);
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)buf[i];
		if (c == '.') {
			if (label_len == 0 || label_len > 63)
				return (FALSE);
			if (buf[i - 1] == '-')
				return (FALSE);
			label_count++;
			label_len = 0;
			continue;
		}
		if (label_len == 0 && c == '-')
			return (FALSE);
		if (!isalnum(c) && c != '-')
			return (FALSE);
		label_len++;
	}
	if (label_len == 0 || label_len > 63)
		return (FALSE);
	if (buf[len - 1] == '-')
		return (FALSE);
	(void)label_count;
	return (TRUE);
}
