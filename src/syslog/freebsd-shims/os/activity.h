/* os/activity.h — FreeBSD shim. Apple activity-tracing is firehose-based
 * (libsystem_trace, closed source). Stub to no-op blocks. */
#ifndef _FREEBSD_SHIM_OS_ACTIVITY_H_
#define _FREEBSD_SHIM_OS_ACTIVITY_H_

typedef void * os_activity_t;

#define OS_ACTIVITY_NONE		((os_activity_t)0)
#define OS_ACTIVITY_CURRENT		((os_activity_t)0)

#define os_activity_initiate(desc, flags, block)	do { block(); } while (0)
#define os_activity_initiate_f(desc, flags, ctx, fn)	(fn)(ctx)
#define os_activity_apply(act, block)			do { block(); } while (0)
#define os_activity_apply_f(act, ctx, fn)		(fn)(ctx)
#define os_activity_label_useraction(name)		(void)0
#define os_activity_create(desc, parent, flags)		OS_ACTIVITY_NONE
#define os_activity_scope(act)				(void)0
#define os_activity_scope_enter(act, state)		(void)0
#define os_activity_scope_leave(state)			(void)0

#endif
