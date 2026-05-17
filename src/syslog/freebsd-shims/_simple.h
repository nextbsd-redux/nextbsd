/*
 * _simple.h — FreeBSD shim for Apple's <_simple.h>.
 *
 * Apple's _simple is a malloc-free string builder used by the
 * crash-time logging path. launchd-842's core.c only references the
 * opaque _SIMPLE_STRING handle type — as the (unused) first parameter
 * of its job_log_bug() / jobmgr_log_bug() os_redirect callbacks — and
 * calls none of the _simple_* builder functions. So this shim just
 * supplies the opaque typedef.
 */
#ifndef _FREEBSD_SHIM__SIMPLE_H_
#define _FREEBSD_SHIM__SIMPLE_H_

typedef struct _SIMPLE_STRING_s *_SIMPLE_STRING;

#endif /* _FREEBSD_SHIM__SIMPLE_H_ */
