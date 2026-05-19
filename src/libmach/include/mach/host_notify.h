/*
 * mach/host_notify.h — Apple-canonical host notification constants.
 *
 * libdispatch's event_kevent.c (2860, 2952) compares incoming MIG
 * reply msgids against HOST_CALENDAR_*_REPLYID and registers for
 * HOST_NOTIFY_CALENDAR_CHANGE via host_request_notification. Values
 * mirror our kernel-side <sys/mach/host_notify.h>.
 */
#ifndef _MACH_HOST_NOTIFY_H_
#define _MACH_HOST_NOTIFY_H_

#ifdef __cplusplus
extern "C" {
#endif

#define HOST_NOTIFY_CALENDAR_CHANGE	0
#define HOST_NOTIFY_TYPE_MAX		0

#define HOST_CALENDAR_CHANGED_REPLYID	950
/* HOST_CALENDAR_SET_REPLYID (951) is defined in libdispatch's
 * event_config.h:258 with the canonical fallback. We mirror it here
 * for consumers that include only this header. */
#ifndef HOST_CALENDAR_SET_REPLYID
#define HOST_CALENDAR_SET_REPLYID	951
#endif

#ifdef __cplusplus
}
#endif

#endif /* !_MACH_HOST_NOTIFY_H_ */
