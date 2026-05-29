/*
 * launchd_early.h — PID-1 early-init helpers. See launchd_early.c.
 */

#ifndef _LAUNCHD_EARLY_H_
#define _LAUNCHD_EARLY_H_

#include <sys/cdefs.h>
#include <stddef.h>

__BEGIN_DECLS

/*
 * Synthesise a per-machine hostname (slug+suffix from SMBIOS+MAC+
 * hostuuid) and sethostname(2) it. If `out` non-NULL and `outsz` > 0,
 * the synthesized name is also copied there. Returns 0 on success,
 * -1 on sethostname(2) failure (errno set).
 */
int launchd_early_sethostname(char *out, size_t outsz);

/*
 * Open /dev/klog and leak the fd so the kernel routes log() / printf()
 * to TOLOG only and skips TOCONS. Returns the fd on success
 * (intentionally leaked, do not close), -1 on open failure.
 */
int launchd_early_open_klog(void);

__END_DECLS

#endif /* _LAUNCHD_EARLY_H_ */
