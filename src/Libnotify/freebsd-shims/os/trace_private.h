/* os/trace_private.h — FreeBSD shim. Apple's tracing macros that
 * record events to the kernel firehose (libsystem_trace). We don't
 * port the firehose; stub all macros to no-op. */
#ifndef _FREEBSD_SHIM_OS_TRACE_PRIVATE_H_
#define _FREEBSD_SHIM_OS_TRACE_PRIVATE_H_

#define os_trace(fmt, ...)			(void)0
#define os_trace_debug(fmt, ...)		(void)0
#define os_trace_info(fmt, ...)			(void)0
#define os_trace_error(fmt, ...)		(void)0
#define os_trace_fault(fmt, ...)		(void)0
#define os_trace_with_payload(...)		(void)0

/* os_trace mode constants used by notifyd to configure tracing.
 * All map to "disabled" since we have no firehose. */
#define OS_TRACE_MODE_OFF			0
#define OS_TRACE_MODE_DISABLE			0
#define OS_TRACE_MODE_DEBUG			0
#define OS_TRACE_MODE_INFO			0
#define OS_TRACE_MODE_STREAM_LIVE		0
#define OS_TRACE_MODE_HISTORY_BUFFER		0

#define os_trace_set_mode(mode)			(void)0
#define os_trace_get_mode()			0

#endif
