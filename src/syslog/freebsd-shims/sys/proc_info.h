/*
 * sys/proc_info.h — FreeBSD shim for Apple's <sys/proc_info.h>.
 *
 * core.c includes this as <System/sys/proc_info.h>, runtime.c as
 * <sys/proc_info.h>. The payload (struct layouts + flavor codes)
 * lives in <System/sys/proc_info.h>; this header just re-includes it
 * so both spellings resolve to the same definitions.
 */
#ifndef _FREEBSD_SHIM_SYS_PROC_INFO_H_
#define _FREEBSD_SHIM_SYS_PROC_INFO_H_

#include <System/sys/proc_info.h>

#endif /* _FREEBSD_SHIM_SYS_PROC_INFO_H_ */
