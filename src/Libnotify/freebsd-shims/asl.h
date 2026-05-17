/* asl.h — FreeBSD shim for libnotify's vestigial ASL references.
 *
 * notify_client.c calls asl_log() / asl_log_message() and references
 * ASL_LEVEL_* constants on its error-path debug log calls. Until real
 * libsystem_asl ships (J1 second half), provide the constants + stub
 * the logging macros to fprintf(stderr). When real ASL lands, the
 * /usr/include/asl.h takes precedence in the include path.
 */
#ifndef _FREEBSD_SHIM_NOTIFY_ASL_H_
#define _FREEBSD_SHIM_NOTIFY_ASL_H_

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

/* Standard ASL priority levels — match syslog(3) LOG_* values. */
#define ASL_LEVEL_EMERG		0
#define ASL_LEVEL_ALERT		1
#define ASL_LEVEL_CRIT		2
#define ASL_LEVEL_ERR		3
#define ASL_LEVEL_WARNING	4
#define ASL_LEVEL_NOTICE	5
#define ASL_LEVEL_INFO		6
#define ASL_LEVEL_DEBUG		7

/* ASL keys for structured messages. */
#define ASL_KEY_MSG		"Message"
#define ASL_KEY_LEVEL		"Level"
#define ASL_KEY_PID		"PID"
#define ASL_KEY_TIME		"Time"
#define ASL_KEY_HOST		"Host"
#define ASL_KEY_SENDER		"Sender"
#define ASL_KEY_FACILITY	"Facility"

/* Opaque object types. */
typedef void * asl_object_t;

#define ASL_TYPE_MSG		1

/* Client API surface used by notify_client.c — stubbed to stderr. */
static inline asl_object_t
asl_open(const char *ident, const char *facility, uint32_t opts)
{
	(void)ident; (void)facility; (void)opts;
	return NULL;
}

static inline void
asl_close(asl_object_t obj) { (void)obj; }

static inline int
asl_log(asl_object_t obj, asl_object_t msg, int level, const char *fmt, ...)
{
	(void)obj; (void)msg;
	va_list ap;
	va_start(ap, fmt);
	(void)fprintf(stderr, "[ASL %d] ", level);
	(void)vfprintf(stderr, fmt, ap);
	(void)fputc('\n', stderr);
	va_end(ap);
	return 0;
}

static inline int
asl_log_message(int level, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	(void)fprintf(stderr, "[ASL %d] ", level);
	(void)vfprintf(stderr, fmt, ap);
	(void)fputc('\n', stderr);
	va_end(ap);
	return 0;
}

#endif
