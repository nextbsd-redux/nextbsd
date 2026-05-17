/*
 * notifyd_freebsd_compat.h — force-included shim for notifyd source.
 *
 * Different from libnotify_freebsd_compat.h (used by the client lib);
 * notifyd uses additional Apple-private constants.
 */
#ifndef _FREEBSD_SHIM_NOTIFYD_COMPAT_H_
#define _FREEBSD_SHIM_NOTIFYD_COMPAT_H_

#include <asl.h>
#include <sys/reboot.h>

/* FreeBSD has RB_HALT_CPU / RB_AUTOBOOT but not RB_PANIC. notifyd
 * uses RB_PANIC in a path that's logically "abort + panic" — map to
 * a regular abort() trigger since FreeBSD reboot() can't panic. */
#ifndef RB_PANIC
#define RB_PANIC	0x40000000	/* not a real FreeBSD flag */
#endif

/* MIG-generated subsystem name. Our MIG emits _notify_ipc_subsystem
 * (from `subsystem notify_ipc` + `serverprefix _`). notifyd's source
 * expects do_notify_subsystem — Apple xcode build renames via some
 * post-processing. Alias here keeps Apple source unchanged. */
#define do_notify_subsystem		_notify_ipc_subsystem

#endif
