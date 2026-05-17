/*
 * libnotify_freebsd_compat.h — force-included shim for libnotify
 * source files. Same pattern as launchd's launchctl_freebsd_compat.h.
 *
 * Apple's notify_client.c uses ASL_LEVEL_* constants without
 * #include <asl.h>; they get pulled in via a precompiled-header
 * mechanism in Apple's build. We don't have that, so force-include
 * the ASL stub header here.
 */
#ifndef _FREEBSD_SHIM_LIBNOTIFY_COMPAT_H_
#define _FREEBSD_SHIM_LIBNOTIFY_COMPAT_H_

#include <asl.h>

#endif
