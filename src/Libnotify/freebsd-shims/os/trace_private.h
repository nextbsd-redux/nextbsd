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

#endif
