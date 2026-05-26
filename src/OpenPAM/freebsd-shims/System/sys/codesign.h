/*
 * <System/sys/codesign.h> — FreeBSD shim for Apple's code-signing
 * syscall interface, used by OpenPAM's pam_start.c +
 * openpam_dynamic.c (csops()) to query/manipulate per-process
 * code-signing flags.
 *
 * FreeBSD doesn't have csops(2) — there is no kernel code-signing
 * substrate (no Library Validation, no platform binaries, no
 * mach-o entitlements). Stub csops() to a no-op that returns
 * success with all flags cleared, and define the CS_OPS_* constants
 * OpenPAM references so the source compiles unmodified.
 *
 * The behavioral effect on FreeBSD: OpenPAM treats every caller
 * as having no code-signing flags set, which means none of the
 * Apple-only code paths in openpam_dynamic.c (Library Validation
 * disable, platform-binary protection) take effect. That's the
 * intended FreeBSD behavior — those features don't exist here.
 *
 * Vendored under src/OpenPAM/freebsd-shims/ — added to CFLAGS via
 * the Makefile's -I, NOT installed to the rootfs.
 */
#ifndef _FREEBSD_SHIMS_SYSTEM_SYS_CODESIGN_H_
#define _FREEBSD_SHIMS_SYSTEM_SYS_CODESIGN_H_

#include <sys/types.h>
#include <string.h>

/* Apple constants OpenPAM actually references. Values don't matter
 * to us — csops() is a no-op — but they need to be defined enums
 * or macros for the source to compile. Values match macOS for
 * traceability. */
#define CS_OPS_STATUS		0
#define CS_OPS_CLEAR_LV		18	/* clear Library Validation */
#define CS_OPS_SET_STATUS	2

/* Apple flag bits OpenPAM may reference. Same rationale. */
#define CS_REQUIRE_LV		0x00002000
#define CS_FORCED_LV		0x00000010
#define CS_INSTALLER		0x00000008
#define CS_VALID		0x00000001
#define CS_HARD			0x00000100
#define CS_KILL			0x00000200

/* csops() — no-op stub. macOS signature:
 *   int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
 *
 * Returns 0 on success. For CS_OPS_STATUS we zero the buffer (no
 * flags set). For CS_OPS_SET_STATUS / CS_OPS_CLEAR_LV we just
 * return success; no kernel state changes (there's no kernel
 * state to change on FreeBSD).
 */
static __inline int
csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize)
{
	(void)pid;
	(void)ops;
	if (useraddr != NULL && usersize > 0)
		(void)memset(useraddr, 0, usersize);
	return (0);
}

#endif /* _FREEBSD_SHIMS_SYSTEM_SYS_CODESIGN_H_ */
