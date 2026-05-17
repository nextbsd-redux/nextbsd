/* copyfile.h — FreeBSD shim. Apple's copyfile(3) is a high-level
 * "copy with metadata" API; FreeBSD has nothing equivalent. aslmanager
 * uses it once to move rotated logs into Archive/. Stub declares the
 * flag constants and the function; real implementation in
 * notifyd_stubs.c uses recursive cp(1) via posix_spawn. */
#ifndef _FREEBSD_SHIM_COPYFILE_H_
#define _FREEBSD_SHIM_COPYFILE_H_

#include <sys/types.h>
#include <stdint.h>

typedef void * copyfile_state_t;
typedef uint32_t copyfile_flags_t;

#define COPYFILE_ACL		(1<<0)
#define COPYFILE_STAT		(1<<1)
#define COPYFILE_XATTR		(1<<2)
#define COPYFILE_DATA		(1<<3)
#define COPYFILE_SECURITY	(COPYFILE_STAT | COPYFILE_ACL)
#define COPYFILE_METADATA	(COPYFILE_SECURITY | COPYFILE_XATTR)
#define COPYFILE_ALL		(COPYFILE_METADATA | COPYFILE_DATA)

#define COPYFILE_RECURSIVE	(1<<15)
#define COPYFILE_CHECK		(1<<16)
#define COPYFILE_EXCL		(1<<17)
#define COPYFILE_NOFOLLOW_SRC	(1<<18)
#define COPYFILE_NOFOLLOW_DST	(1<<19)
#define COPYFILE_MOVE		(1<<20)
#define COPYFILE_UNLINK		(1<<21)
#define COPYFILE_NOFOLLOW	(COPYFILE_NOFOLLOW_SRC | COPYFILE_NOFOLLOW_DST)

int copyfile(const char *from, const char *to, copyfile_state_t state,
    copyfile_flags_t flags);

#endif
