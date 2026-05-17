/*
 * os/log.h — FreeBSD shim for Apple's <os/log.h>.
 *
 * Apple's os_log family targets the kernel firehose (libsystem_trace),
 * which is closed-source and we're not porting. Stub the macros to
 * fprintf(stderr, ...) at compile time so Apple source compiles and
 * runs with degraded structured-logging.
 *
 * Once full ASL is up, this can be updated to expand os_log macros
 * to asl_log() calls (additive change; no caller churn).
 */
#ifndef _FREEBSD_SHIM_OS_LOG_H_
#define _FREEBSD_SHIM_OS_LOG_H_

#include <stdio.h>
#include <stdint.h>

typedef void * os_log_t;
typedef uint8_t  os_log_type_t;
typedef uint64_t os_activity_id_t;

#define OS_LOG_DEFAULT		((os_log_t)0)
#define OS_LOG_DISABLED		((os_log_t)-1)

/* Apple os_log_type_t values — structured equivalents of syslog
 * levels. Numeric values match Apple's enum. */
#define OS_LOG_TYPE_DEFAULT	0x00
#define OS_LOG_TYPE_INFO	0x01
#define OS_LOG_TYPE_DEBUG	0x02
#define OS_LOG_TYPE_ERROR	0x10
#define OS_LOG_TYPE_FAULT	0x11

#define os_log(log, fmt, ...)		(void)fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#define os_log_info(log, fmt, ...)	(void)fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#define os_log_debug(log, fmt, ...)	(void)0
#define os_log_error(log, fmt, ...)	(void)fprintf(stderr, "ERROR: " fmt "\n", ##__VA_ARGS__)
#define os_log_fault(log, fmt, ...)	(void)fprintf(stderr, "FAULT: " fmt "\n", ##__VA_ARGS__)
#define os_log_with_type(log, t, fmt, ...) (void)fprintf(stderr, fmt "\n", ##__VA_ARGS__)

#define os_log_create(subsystem, category)	OS_LOG_DEFAULT

#endif /* _FREEBSD_SHIM_OS_LOG_H_ */
