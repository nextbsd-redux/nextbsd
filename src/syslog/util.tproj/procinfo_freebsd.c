/*
 * procinfo_freebsd.c — FreeBSD implementation of syslog(1)'s
 * procinfo() helper. Walls the FreeBSD <sys/user.h> sysctl walk
 * into its own TU. Compiled via a Makefile-level custom rule with
 * minimal CFLAGS so the force-included libasl compat shim — which
 * transitively pulls libmach's mach/* headers (vm_map_t /
 * boolean_t typedef collisions with FreeBSD's vm/vm.h) — is not
 * in effect for this file.
 */
#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* Match the return-code constants from util.tproj/syslog.c. */
#define PROC_NOT_FOUND		-1
#define PROC_NOT_UNIQUE		-2

int
procinfo_freebsd(char *pname, int *pid, int *uid)
{
	int mib[4];
	int i, status, nprocs;
	size_t miblen, size;
	struct kinfo_proc *procs, *newprocs;

	size = 0;
	procs = NULL;

	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_PROC;
	mib[3] = 0;
	miblen = 3;

	status = sysctl(mib, miblen, NULL, &size, NULL, 0);
	do {
		size += size / 10;
		newprocs = reallocf(procs, size);
		if (newprocs == NULL) {
			if (procs != NULL) free(procs);
			return PROC_NOT_FOUND;
		}
		procs = newprocs;
		status = sysctl(mib, miblen, procs, &size, NULL, 0);
	} while ((status == -1) && (errno == ENOMEM));

	if (status == -1) {
		if (procs != NULL) free(procs);
		return PROC_NOT_FOUND;
	}
	if (size % sizeof(struct kinfo_proc) != 0) {
		if (procs != NULL) free(procs);
		return PROC_NOT_FOUND;
	}
	if (procs == NULL) return PROC_NOT_FOUND;

	nprocs = size / sizeof(struct kinfo_proc);

	if (pname == NULL) {
		for (i = 0; i < nprocs; i++) {
			if (*pid == procs[i].ki_pid) {
				*uid = procs[i].ki_uid;
				free(procs);
				return 0;
			}
		}
		free(procs);
		return PROC_NOT_FOUND;
	}

	*pid = PROC_NOT_FOUND;
	for (i = 0; i < nprocs; i++) {
		if (!strcmp(procs[i].ki_comm, pname)) {
			if (*pid != PROC_NOT_FOUND) {
				free(procs);
				return PROC_NOT_UNIQUE;
			}
			*pid = procs[i].ki_pid;
			*uid = procs[i].ki_uid;
		}
	}

	free(procs);
	if (*pid == PROC_NOT_FOUND) return PROC_NOT_FOUND;
	return 0;
}
