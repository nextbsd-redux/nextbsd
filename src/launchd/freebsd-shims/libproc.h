/*
 * libproc.h — FreeBSD shim for Apple's <libproc.h>.
 *
 * Apple's libproc is the userland wrapper over the proc_info(2)
 * syscall family. launchd-842 reaches for two subsets:
 *   - libvproc.c: dirty-process-tracking — proc_track_dirty() /
 *     proc_set_dirty() / proc_get_dirty() — XNU's "this process has
 *     unsaved state" mechanism for jetsam / sudden-termination.
 *   - core.c / runtime.c: process introspection + lifecycle —
 *     proc_listallpids(), proc_list{child,pgrp}pids(), proc_pidinfo(),
 *     proc_pid_rusage(), proc_terminate().
 *
 * FreeBSD has no proc_info(2). The dirty-tracking calls are no-op
 * successes (no jetsam to gate). The introspection calls are stubs
 * that report "no data" (return 0) so launchd's process walks come
 * up empty rather than reading real FreeBSD process state — a native
 * sysctl(KERN_PROC) port is the proper follow-up. The one exception
 * is proc_terminate(), which maps cleanly onto kill(2). One-shot
 * stderr notes fire so the stubbed gaps stay visible.
 */
#ifndef _LIBPROC_H_SHIM_
#define _LIBPROC_H_SHIM_

#include <sys/types.h>
#include <signal.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/proc_info.h>	/* rusage_info_t + the proc_* struct vocabulary */
#include <sys/sysctl.h>		/* sysctl(KERN_PROC_PID) — proc_pidinfo backing */
#include <sys/user.h>		/* struct kinfo_proc */

/* proc_track_dirty() flags — Apple's <sys/proc_info.h> values. */
#define PROC_DIRTY_TRACK		0x1
#define PROC_DIRTY_ALLOW_IDLE_EXIT	0x2
#define PROC_DIRTY_DEFER		0x4
#define PROC_DIRTY_LAUNCH_IN_PROGRESS	0x8

/* proc_get_dirty() result flags. */
#define PROC_DIRTY_TRACKED		0x1
#define PROC_DIRTY_ALLOWS_IDLE_EXIT	0x2
#define PROC_DIRTY_IS_DIRTY		0x4
#define PROC_DIRTY_LAUNCH_IS_IN_PROGRESS 0x8

#ifndef _LIBPROC_SHIM_WARN
#define _LIBPROC_SHIM_WARN(fn) do { \
	static int _w = 0; \
	if (!_w) { fprintf(stderr, \
	    "libproc: " fn ": stub (no proc_info(2) on FreeBSD)\n"); \
	    _w = 1; } \
} while (0)
#endif

static __inline int
proc_track_dirty(pid_t pid, uint32_t flags)
{
	(void)pid; (void)flags;
	_LIBPROC_SHIM_WARN("proc_track_dirty");
	return 0;
}

static __inline int
proc_set_dirty(pid_t pid, bool dirty)
{
	(void)pid; (void)dirty;
	_LIBPROC_SHIM_WARN("proc_set_dirty");
	return 0;
}

static __inline int
proc_get_dirty(pid_t pid, uint32_t *flags)
{
	(void)pid;
	if (flags != NULL)
		*flags = 0;
	_LIBPROC_SHIM_WARN("proc_get_dirty");
	return 0;
}

/*
 * Process introspection. proc_info(2) doesn't exist on FreeBSD, so
 * these report "nothing": the pid-list calls return 0 bytes written
 * (an empty list, not an error), proc_pidinfo() / proc_pid_rusage()
 * return 0 ("no info"). launchd's callers handle the empty/zero case
 * — they walk zero pids and skip the per-pid info. A native port
 * would back these with sysctl(KERN_PROC).
 */
static __inline int
proc_listallpids(void *buffer, int buffersize)
{
	(void)buffer; (void)buffersize;
	_LIBPROC_SHIM_WARN("proc_listallpids");
	return 0;
}

static __inline int
proc_listchildpids(pid_t ppid, void *buffer, int buffersize)
{
	(void)ppid; (void)buffer; (void)buffersize;
	_LIBPROC_SHIM_WARN("proc_listchildpids");
	return 0;
}

static __inline int
proc_listpgrppids(pid_t pgrpid, void *buffer, int buffersize)
{
	(void)pgrpid; (void)buffer; (void)buffersize;
	_LIBPROC_SHIM_WARN("proc_listpgrppids");
	return 0;
}

/*
 * proc_pidinfo() — native FreeBSD port via sysctl(KERN_PROC_PID).
 *
 * Previously a stub returning 0. That made launchd's job_new_anonymous()
 * treat every anonymous caller as "process gone" and return j=NULL, so
 * every bootstrap_look_up() from a non-launchd process failed with
 * BOOTSTRAP_NO_MEMORY. Backed by sysctl now (the two flavors launchd
 * actually uses: PROC_PIDT_SHORTBSDINFO in core.c, and
 * PROC_PIDUNIQIDENTIFIERINFO in runtime.c). Returns the bytes written
 * (Apple's contract), or 0 on failure with errno set by sysctl.
 */
static __inline int
proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize)
{
	struct kinfo_proc kp;
	size_t kplen = sizeof(kp);
	int mib[4];

	(void)arg;
	if (buffer == NULL || pid <= 0)
		return 0;

	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_PID;
	mib[3] = pid;

	switch (flavor) {
	case PROC_PIDT_SHORTBSDINFO: {
		struct proc_bsdshortinfo si;

		if (buffersize < (int)sizeof(si))
			return 0;
		if (sysctl(mib, 4, &kp, &kplen, NULL, 0) != 0 || kplen == 0)
			return 0;	/* sysctl sets errno (ESRCH if pid gone) */
		memset(&si, 0, sizeof(si));
		si.pbsi_pid    = (uint32_t)kp.ki_pid;
		si.pbsi_ppid   = (uint32_t)kp.ki_ppid;
		si.pbsi_pgid   = (uint32_t)kp.ki_pgid;
		si.pbsi_status = (uint32_t)kp.ki_stat;
		strlcpy(si.pbsi_comm, kp.ki_comm, sizeof(si.pbsi_comm));
		si.pbsi_uid    = kp.ki_uid;
		si.pbsi_gid    = kp.ki_groups[0];
		si.pbsi_ruid   = kp.ki_ruid;
		si.pbsi_rgid   = kp.ki_rgid;
		si.pbsi_svuid  = kp.ki_svuid;
		si.pbsi_svgid  = kp.ki_svgid;
		memcpy(buffer, &si, sizeof(si));
		return (int)sizeof(si);
	}
	case PROC_PIDUNIQIDENTIFIERINFO: {
		struct proc_uniqidentifierinfo ui;

		if (buffersize < (int)sizeof(ui))
			return 0;
		if (sysctl(mib, 4, &kp, &kplen, NULL, 0) != 0 || kplen == 0)
			return 0;
		memset(&ui, 0, sizeof(ui));
		ui.p_uniqueid  = (uint64_t)kp.ki_pid;
		ui.p_puniqueid = (uint64_t)kp.ki_ppid;
		memcpy(buffer, &ui, sizeof(ui));
		return (int)sizeof(ui);
	}
	default:
		_LIBPROC_SHIM_WARN("proc_pidinfo");
		return 0;
	}
}

static __inline int
proc_pid_rusage(pid_t pid, int flavor, rusage_info_t *buffer)
{
	(void)pid; (void)flavor; (void)buffer;
	_LIBPROC_SHIM_WARN("proc_pid_rusage");
	return -1;
}

/*
 * proc_terminate() — the one call with a clean FreeBSD mapping.
 * Apple's proc_terminate() asks the kernel to end a process and
 * reports which signal it used; kill(pid, SIGTERM) is the direct
 * equivalent.
 */
static __inline int
proc_terminate(pid_t pid, int *sig)
{
	int s = SIGTERM;
	if (sig != NULL)
		*sig = s;
	return kill(pid, s);
}

#endif /* !_LIBPROC_H_SHIM_ */
