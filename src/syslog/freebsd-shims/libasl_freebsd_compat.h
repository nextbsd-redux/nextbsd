/*
 * libasl_freebsd_compat.h — force-included shim for libsystem_asl
 * source files. Same pattern as launchctl's launchctl_freebsd_compat.h.
 *
 * Apple's libsystem_asl source uses a few syslog facility constants
 * that FreeBSD's <sys/syslog.h> doesn't ship (LOG_INSTALL, LOG_LAUNCHD,
 * LOG_NETINFO, LOG_REMOTEAUTH, LOG_RAS). Apple defined these in
 * their syslog.h variant. Define them here at the same numeric values
 * Apple uses, so libsystem_asl compiles unchanged.
 */
#ifndef _FREEBSD_SHIM_LIBASL_COMPAT_H_
#define _FREEBSD_SHIM_LIBASL_COMPAT_H_

#include <sys/syslog.h>

/* Apple-only syslog facility codes. Numeric values match Apple's
 * <sys/syslog.h>. */
#ifndef LOG_NETINFO
#define LOG_NETINFO	(12<<3)
#endif
#ifndef LOG_REMOTEAUTH
#define LOG_REMOTEAUTH	(13<<3)
#endif
#ifndef LOG_INSTALL
#define LOG_INSTALL	(14<<3)
#endif
#ifndef LOG_RAS
#define LOG_RAS		(15<<3)
#endif
#ifndef LOG_LAUNCHD
#define LOG_LAUNCHD	(24<<3)
#endif

/* Apple-only ACL constants — asl_file.c uses ACL_EXTENDED_ALLOW
 * to grant read access to ASL store files. FreeBSD's POSIX.1e ACLs
 * use a different model (no extended ALLOW/DENY entries) — the
 * code path is essentially "best-effort"; setting these constants
 * to values FreeBSD ignores keeps the source compiling and the
 * store creation falls back to plain mode bits. */
#ifndef ACL_EXTENDED_ALLOW
#define ACL_EXTENDED_ALLOW	1
#endif
#ifndef ACL_EXTENDED_DENY
#define ACL_EXTENDED_DENY	2
#endif

/* Apple OSSpinLock — deprecated even on macOS. Stub to int; we
 * don't take perf-sensitive contended locks in the boot path. */
typedef volatile int OSSpinLock;
#define OS_SPINLOCK_INIT	0
#define OSSpinLockLock(l)	(void)__sync_lock_test_and_set((l), 1)
#define OSSpinLockUnlock(l)	__sync_lock_release(l)
#define OSSpinLockTry(l)	(__sync_bool_compare_and_swap((l), 0, 1))

/* setiopolicy_np options — Apple I/O policy hints (lower a daemon's
 * disk priority). FreeBSD has no equivalent; stub the call. */
#define IOPOL_TYPE_DISK		0
#define IOPOL_SCOPE_PROCESS	1
#define IOPOL_SCOPE_THREAD	2
#define IOPOL_PASSIVE		1
#define IOPOL_THROTTLE		2
#define IOPOL_UTILITY		3
#define IOPOL_STANDARD		4

static inline int
setiopolicy_np(int type, int scope, int policy) {
	(void)type; (void)scope; (void)policy;
	return 0;
}

/* Apple quarantine flag QTN_FLAG_HARD (subset of QTN_FLAG_HARD_QUARANTINE
 * from quarantine.h) - used by syslogd in qtn_proc_init path. */
#ifndef QTN_FLAG_HARD
#define QTN_FLAG_HARD	QTN_FLAG_HARD_QUARANTINE
#endif

#endif
