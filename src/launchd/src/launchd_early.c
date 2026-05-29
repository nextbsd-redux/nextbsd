/*
 * launchd_early.c — freebsd-launchd-mach PID-1 early-init helpers.
 *
 * Two boot-readiness primitives the LaunchDaemon scan needs done
 * BEFORE it dispatches any plist:
 *
 *  1. launchd_early_sethostname() — synthesize a per-machine hostname
 *     from SMBIOS + first NIC MAC + kern.hostuuid, sethostname(2) it.
 *     getty (system_cmds/getty/main.c:202) reads gethostname() once
 *     at process start and caches it in a process-global; without
 *     this early set, getty's first login banner shows the FreeBSD
 *     default "Amnesiac" until the user presses return (which exits
 *     getty, launchd respawns, and the new getty reads the updated
 *     hostname). hostnamed's own boot-time sethostname runs too late:
 *     launchd dispatches hostnamed and getty in parallel, getty wins
 *     the race. PID 1 doing the synth+sethostname BEFORE plist
 *     dispatch is the cleanest fix — hostnamed then refines the
 *     value later through its full SCPrefs/DHCP/PTR/mDNS chain.
 *
 *  2. launchd_early_open_klog() — open(/dev/klog, O_RDONLY) and
 *     leak the fd. kern/subr_log.c:104-124 logopen() flips log_open
 *     to 1 on first open; once set, kern/subr_prf.c:321 vlog() routes
 *     to TOLOG only, NOT TOCONS|TOLOG. Without an early klog reader,
 *     kernel printf()s (e.g. nd6_dad_timer messages) bleed into
 *     /dev/console mid-getty-prompt. Stock FreeBSD's syslogd opens
 *     /dev/klog at startup and the kernel goes quiet on console;
 *     our syslogd has klog_in deactivated (syslog/syslogd.tproj/
 *     syslogd.c:90, task #41 libdispatch+Mach deadlock workaround),
 *     so PID 1 takes ownership of the log_open flip itself. The fd
 *     is intentionally leaked — we just need logopen() to fire once.
 *
 * Synthesis machinery is duplicated from
 * src/hostnamed/freebsd-shim/shim.c (sh_derive_slug, sh_derive_suffix,
 * sh_first_nic_mac, sh_sanitize_slug, sh_last_alnum_suffix,
 * sh_serial_is_placeholder, sh_read_kenv, sh_read_sysctl_str) instead
 * of linked — PID 1 is the foundation, every transitive .so it pulls
 * is one more thing that has to be present and loadable on a degraded
 * boot. Direct C means no CoreFoundation, no SCDS, no Libnotify;
 * only libc + libthr.
 *
 * Both helpers are best-effort: failures log to launchd_console and
 * boot continues. They are not the sole or final hostname / log
 * routing surface — they're a quality-of-life floor.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <net/if_dl.h>
#include <net/if_types.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <kenv.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define EARLY_SLUG_MAX		48
#define EARLY_SUFFIX_LEN	6
#define EARLY_NAME_MAX		64	/* DNS label limit */

#include "launchd_early.h"

/* --- synthesis machinery (mirrors hostnamed's shim) ----------------- */

static int
early_read_kenv(const char *name, char *out, size_t outsz)
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

static int
early_read_sysctl_str(const char *name, char *out, size_t outsz)
{
	size_t n = outsz;

	if (sysctlbyname(name, out, &n, NULL, 0) != 0 || n == 0)
		return (-1);
	out[outsz - 1] = '\0';
	if (n > outsz)
		n = outsz;
	return ((int)n);
}

static size_t
early_sanitize_slug(char *s)
{
	size_t i, j;
	int prev_dash;

	if (s == NULL)
		return (0);
	j = 0;
	prev_dash = 1;
	for (i = 0; s[i] != '\0' && j < EARLY_SLUG_MAX; i++) {
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

static int
early_serial_is_placeholder(const char *s)
{
	static const char *const placeholders[] = {
		"", "None", "To be filled by O.E.M.",
		"To Be Filled By O.E.M.", "Default string", "0",
		"0123456789", "System Serial Number", "Not Specified",
		NULL,
	};
	int i;

	if (s == NULL)
		return (1);
	for (i = 0; placeholders[i] != NULL; i++)
		if (strcmp(s, placeholders[i]) == 0)
			return (1);
	return (0);
}

static int
early_last_alnum_suffix(const char *src, char *out)
{
	char filtered[128];
	size_t i, j, n;

	j = 0;
	for (i = 0; src[i] != '\0' && j < sizeof(filtered) - 1; i++)
		if (isalnum((unsigned char)src[i]))
			filtered[j++] = src[i];
	filtered[j] = '\0';
	if (j < EARLY_SUFFIX_LEN)
		return (-1);
	n = j - EARLY_SUFFIX_LEN;
	(void)memcpy(out, filtered + n, EARLY_SUFFIX_LEN);
	out[EARLY_SUFFIX_LEN] = '\0';
	return (0);
}

static int
early_first_nic_mac(uint8_t mac[6])
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
		for (i = 0; i < 6; i++)
			if (((const uint8_t *)LLADDR(dl))[i] != 0) {
				zero = 0;
				break;
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

static int
early_derive_suffix(char out[EARLY_SUFFIX_LEN + 1])
{
	char buf[256];
	uint8_t mac[6];

	if (early_read_kenv("smbios.system.serial", buf, sizeof(buf)) > 0 &&
	    !early_serial_is_placeholder(buf) &&
	    early_last_alnum_suffix(buf, out) == 0)
		return (0);
	if (early_first_nic_mac(mac) == 0) {
		(void)snprintf(out, EARLY_SUFFIX_LEN + 1, "%02x%02x%02x",
		    mac[3], mac[4], mac[5]);
		return (0);
	}
	if (early_read_sysctl_str("kern.hostuuid", buf, sizeof(buf)) > 0) {
		char filtered[64];
		size_t i, j;
		j = 0;
		for (i = 0; buf[i] != '\0' && j < sizeof(filtered) - 1; i++) {
			unsigned char c = (unsigned char)buf[i];
			if (isxdigit(c))
				filtered[j++] = (char)tolower(c);
		}
		filtered[j] = '\0';
		if (j >= EARLY_SUFFIX_LEN) {
			(void)memcpy(out, filtered, EARLY_SUFFIX_LEN);
			out[EARLY_SUFFIX_LEN] = '\0';
			return (0);
		}
	}
	return (-1);
}

static void
early_derive_slug(char *out, size_t outsz)
{
	char buf[256];

	if (early_read_kenv("smbios.system.version", buf, sizeof(buf)) > 0) {
		(void)strncpy(out, buf, outsz - 1);
		out[outsz - 1] = '\0';
		(void)early_sanitize_slug(out);
		if (out[0] != '\0')
			return;
	}
	if (early_read_kenv("smbios.system.product", buf, sizeof(buf)) > 0) {
		(void)strncpy(out, buf, outsz - 1);
		out[outsz - 1] = '\0';
		(void)early_sanitize_slug(out);
		if (out[0] != '\0')
			return;
	}
	(void)strncpy(out, "freebsd", outsz - 1);
	out[outsz - 1] = '\0';
}

int
launchd_early_sethostname(char *out, size_t outsz)
{
	char slug[EARLY_SLUG_MAX + 1];
	char suffix[EARLY_SUFFIX_LEN + 1];
	char name[EARLY_NAME_MAX + 1];

	early_derive_slug(slug, sizeof(slug));
	if (early_derive_suffix(suffix) != 0) {
		(void)strncpy(name, "freebsd", sizeof(name) - 1);
		name[sizeof(name) - 1] = '\0';
	} else {
		(void)snprintf(name, sizeof(name), "%s-%s", slug, suffix);
	}
	if (sethostname(name, (int)strlen(name)) != 0)
		return (-1);
	if (out != NULL && outsz > 0) {
		(void)strncpy(out, name, outsz - 1);
		out[outsz - 1] = '\0';
	}
	return (0);
}

int
launchd_early_open_klog(void)
{
	int fd;

	fd = open("/dev/klog", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return (-1);
	/*
	 * Deliberately leak the fd. kern.log_open is set by the first
	 * open(2) on /dev/klog (subr_log.c:104), and we want it to stay
	 * 1 for the lifetime of PID 1. We don't drain — kernel msgbuf
	 * sizes itself and rolls without back-pressure when nobody
	 * reads. Once syslogd's klog_in module is re-enabled (task #41),
	 * it will open a second reader; multi-reader /dev/klog is
	 * supported by FreeBSD's logread() (subr_log.c).
	 */
	return (fd);
}
