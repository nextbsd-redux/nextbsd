/* os/log_simple_private.h — FreeBSD shim. Apple-private simple-log API. */
#ifndef _FREEBSD_SHIM_OS_LOG_SIMPLE_PRIVATE_H_
#define _FREEBSD_SHIM_OS_LOG_SIMPLE_PRIVATE_H_

#include <stdio.h>
#include <stdarg.h>

/* Apple os_log_simple_type_t — enum for the message kind. */
typedef enum {
	OS_LOG_SIMPLE_TYPE_DEFAULT	= 0,
	OS_LOG_SIMPLE_TYPE_INFO		= 1,
	OS_LOG_SIMPLE_TYPE_DEBUG	= 2,
	OS_LOG_SIMPLE_TYPE_ERROR	= 16,
	OS_LOG_SIMPLE_TYPE_FAULT	= 17,
} os_log_simple_type_t;

/* Apple's os_log_simple* prototypes — stubbed to vfprintf(stderr). */
#define os_log_simple(level, msg)		(void)fprintf(stderr, "%s\n", (msg))
#define os_log_simple_with_pid(level, pid, msg)	(void)fprintf(stderr, "[%d] %s\n", (int)(pid), (msg))
#define os_log_simple_type(type, subsystem, fmt, ...) \
	(void)fprintf(stderr, "[%s] " fmt "\n", (subsystem), ##__VA_ARGS__)
#define os_log_simple_with_subsystem(level, subsystem, fmt, ...) \
	(void)fprintf(stderr, "[%s] " fmt "\n", (subsystem), ##__VA_ARGS__)

#endif
