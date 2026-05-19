/*
 * mach/error.h — Apple-canonical error-packing macros.
 *
 * Layout (matches XNU and our kernel-side <sys/mach/error.h>):
 *
 *   bits 31..26  | bits 25..14  | bits 13..0
 *   system (6)   | subsystem(12)| code (14)
 *
 * The `err_system`, `err_sub`, `err_get_*`, and `code_emask` macros
 * here let libdispatch (mach.c:549-561, _dispatch_mach_msg_get_reason)
 * decode and synthesize mach_error_t values without pulling in
 * kernel-internal headers.
 *
 * Return-type note. Apple's macros return signed int (mach_error_t),
 * which trips -Wsign-conversion when libdispatch assigns the result
 * to unsigned long. err_get_code's return is cast to unsigned long
 * here so libdispatch's `return err_get_code(err);` (mach.c:561)
 * compiles clean. The other macros stay int — bit-shift / mask ops
 * that stay in mach_error_t's domain.
 */
#ifndef _MACH_ERROR_H_
#define _MACH_ERROR_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef int mach_error_t;

#define err_none		((mach_error_t)0)

#define err_system(x)		(((x) & 0x3f) << 26)
#define err_sub(x)		(((x) & 0xfff) << 14)

#define err_get_system(err)	(((err) >> 26) & 0x3f)
#define err_get_sub(err)	(((err) >> 14) & 0xfff)
#define err_get_code(err)	((unsigned long)((err) & 0x3fff))

#define system_emask		(err_system(0x3f))
#define sub_emask		(err_sub(0xfff))
#define code_emask		(0x3fff)

#define err_kern		err_system(0x0)		/* kernel */
#define err_us			err_system(0x1)		/* user space library */
#define err_server		err_system(0x2)		/* user space servers */
#define err_ipc			err_system(0x3)		/* old ipc errors */
#define err_mach_ipc		err_system(0x4)		/* mach-ipc errors */
#define err_dipc		err_system(0x7)		/* distributed ipc */
#define err_local		err_system(0x3e)	/* user defined errors */

#ifdef __cplusplus
}
#endif

#endif /* !_MACH_ERROR_H_ */
