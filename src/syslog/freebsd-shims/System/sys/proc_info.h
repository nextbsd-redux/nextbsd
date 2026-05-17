/*
 * System/sys/proc_info.h — FreeBSD shim for Apple's
 * <sys/proc_info.h>.
 *
 * Apple's <sys/proc_info.h> is the type vocabulary for the
 * proc_info(2) syscall family (struct proc_bsdinfo, the rusage_info
 * generations, the PROC_PID* flavor codes). launchd-842's core.c +
 * runtime.c declare variables of these types and pass the flavor
 * codes to proc_pidinfo() / proc_pid_rusage().
 *
 * FreeBSD has no proc_info(2). This shim supplies the struct layouts
 * and flavor constants so the Apple source compiles; the actual
 * proc_pidinfo() / proc_pid_rusage() wrappers (in the <libproc.h>
 * shim) are stubs that report "no data", so launchd's process
 * introspection degrades gracefully rather than reading real FreeBSD
 * process state — that's a later, native (sysctl KERN_PROC) port.
 *
 * Struct layouts follow Apple's canonical definitions; only the
 * members launchd-842 actually reads are guaranteed meaningful.
 *
 * This header is the single source of truth — <sys/proc_info.h>
 * (no System/ prefix, included by runtime.c) just re-includes it.
 */
#ifndef _FREEBSD_SHIM_SYSTEM_SYS_PROC_INFO_H_
#define _FREEBSD_SHIM_SYSTEM_SYS_PROC_INFO_H_

#include <sys/types.h>
#include <sys/param.h>		/* MAXCOMLEN */
#include <stdint.h>

struct proc_bsdinfo {
	uint32_t	pbi_flags;
	uint32_t	pbi_status;
	uint32_t	pbi_xstatus;
	uint32_t	pbi_pid;
	uint32_t	pbi_ppid;
	uid_t		pbi_uid;
	gid_t		pbi_gid;
	uid_t		pbi_ruid;
	gid_t		pbi_rgid;
	uid_t		pbi_svuid;
	gid_t		pbi_svgid;
	uint32_t	pbi_rfu;
	char		pbi_comm[MAXCOMLEN + 1];
	char		pbi_name[2 * MAXCOMLEN + 1];
	uint32_t	pbi_nfiles;
	uint32_t	pbi_pgid;
	uint32_t	pbi_pjobc;
	uint32_t	e_tdev;
	uint32_t	e_tpgid;
	int32_t		pbi_nice;
	uint64_t	pbi_start_tvsec;
	uint64_t	pbi_start_tvusec;
};

struct proc_bsdshortinfo {
	uint32_t	pbsi_pid;
	uint32_t	pbsi_ppid;
	uint32_t	pbsi_pgid;
	uint32_t	pbsi_status;
	char		pbsi_comm[MAXCOMLEN + 1];
	uint32_t	pbsi_flags;
	uid_t		pbsi_uid;
	gid_t		pbsi_gid;
	uid_t		pbsi_ruid;
	gid_t		pbsi_rgid;
	uid_t		pbsi_svuid;
	gid_t		pbsi_svgid;
	uint32_t	pbsi_rfu;
};

struct proc_uniqidentifierinfo {
	uint8_t		p_uuid[16];
	uint64_t	p_uniqueid;
	uint64_t	p_puniqueid;
	uint64_t	p_reserve2;
	uint64_t	p_reserve3;
	uint64_t	p_reserve4;
};

struct proc_bsdinfowithuniqid {
	struct proc_bsdinfo		pbsd;
	struct proc_uniqidentifierinfo	p_uniqidentifier;
};

/* proc_pidinfo() flavors + their buffer sizes. */
#define PROC_PIDT_SHORTBSDINFO		13
#define PROC_PIDT_SHORTBSDINFO_SIZE	(sizeof(struct proc_bsdshortinfo))
#define PROC_PIDT_BSDINFOWITHUNIQID	16
#define PROC_PIDT_BSDINFOWITHUNIQID_SIZE (sizeof(struct proc_bsdinfowithuniqid))
#define PROC_PIDUNIQIDENTIFIERINFO	17
#define PROC_PIDUNIQIDENTIFIERINFO_SIZE	(sizeof(struct proc_uniqidentifierinfo))

/* proc_setpcontrol() controls. */
#define PROC_SETPC_NONE			0
#define PROC_SETPC_THROTTLE		1
#define PROC_SETPC_SUSPEND		2
#define PROC_SETPC_TERMINATE		3

/* rusage_info generations. proc_pid_rusage() fills these. */
struct rusage_info_v0 {
	uint8_t		ri_uuid[16];
	uint64_t	ri_user_time;
	uint64_t	ri_system_time;
	uint64_t	ri_pkg_idle_wkups;
	uint64_t	ri_interrupt_wkups;
	uint64_t	ri_pageins;
	uint64_t	ri_wired_size;
	uint64_t	ri_resident_size;
	uint64_t	ri_phys_footprint;
	uint64_t	ri_proc_start_abstime;
	uint64_t	ri_proc_exit_abstime;
};

struct rusage_info_v1 {
	uint8_t		ri_uuid[16];
	uint64_t	ri_user_time;
	uint64_t	ri_system_time;
	uint64_t	ri_pkg_idle_wkups;
	uint64_t	ri_interrupt_wkups;
	uint64_t	ri_pageins;
	uint64_t	ri_wired_size;
	uint64_t	ri_resident_size;
	uint64_t	ri_phys_footprint;
	uint64_t	ri_proc_start_abstime;
	uint64_t	ri_proc_exit_abstime;
	uint64_t	ri_child_user_time;
	uint64_t	ri_child_system_time;
	uint64_t	ri_child_pkg_idle_wkups;
	uint64_t	ri_child_interrupt_wkups;
	uint64_t	ri_child_pageins;
	uint64_t	ri_child_elapsed_abstime;
};

typedef void *rusage_info_t;

#define RUSAGE_INFO_V0	0
#define RUSAGE_INFO_V1	1

/*
 * Process status / flag constants. FreeBSD's <sys/proc.h> defines
 * these too (SZOMB / P_SUGID / P_EXEC) but only inside _KERNEL —
 * launchd-842 reads them in userland from proc_bsdshortinfo's
 * pbsi_status / pbsi_flags. Values match FreeBSD's kernel headers so
 * once proc_pidinfo() is backed by sysctl(KERN_PROC) the comparisons
 * stay correct.
 */
#ifndef SZOMB
#define SZOMB		5		/* awaiting parent collection */
#endif
#ifndef P_SUGID
#define P_SUGID		0x00000100	/* has set-id privileges */
#endif
#ifndef P_EXEC
#define P_EXEC		0x00004000	/* has called exec */
#endif

#endif /* _FREEBSD_SHIM_SYSTEM_SYS_PROC_INFO_H_ */
