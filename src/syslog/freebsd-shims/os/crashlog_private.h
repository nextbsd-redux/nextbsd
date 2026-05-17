/* os/crashlog_private.h — FreeBSD shim. Apple's CrashReporter
 * annotations. Stub to no-ops. */
#ifndef _FREEBSD_SHIM_OS_CRASHLOG_PRIVATE_H_
#define _FREEBSD_SHIM_OS_CRASHLOG_PRIVATE_H_

#define CRASH_INFO_MESSAGE(msg)			(void)0
#define os_set_crash_message(msg)		(void)0
#define os_set_crash_action(action)		(void)0

#endif
