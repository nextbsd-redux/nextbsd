/*
 * asl_stubs.c — runtime symbol stubs for libsystem_asl.so.1.
 *
 * Apple's libsystem_asl source references a handful of os_activity /
 * os_log private functions that on macOS live in libsystem_trace /
 * libsystem_darwin. We don't ship those libs; stubs let dlopen-time
 * resolution succeed for any consumer that loads libsystem_asl.
 *
 * Linked directly into libsystem_asl.so.1 via Makefile SRCS so the
 * symbol is exposed at link time, not pushed down to per-consumer
 * notifyd_stubs.c / future-libsystem_asl_consumer_stubs.c.
 */
#include <stdint.h>
#include <stddef.h>

typedef uint64_t os_activity_id_t;
typedef void *os_activity_t;

/* os_activity_get_identifier — return current activity ID. We have
 * no activity tracking; return 0 to mean "no activity context". The
 * parent_id out-param (if non-NULL) is also set to 0. */
os_activity_id_t
os_activity_get_identifier(os_activity_t activity, os_activity_id_t *parent_id)
{
	(void)activity;
	if (parent_id) *parent_id = 0;
	return 0;
}

/* os_log_shim_enabled — is the os_log → ASL shim active for this
 * subsystem? We don't shim; always return false so callers take
 * the direct-to-syslogd path. */
int
os_log_shim_enabled(void *log)
{
	(void)log;
	return 0;
}

/* os_log_with_args_4syslog — the shim that os_log uses when ASL is
 * active. We're not active, but if anyone reaches here just no-op. */
void
os_log_with_args_4syslog(void *log, uint8_t type, const char *format,
    void *args, void *ret_addr)
{
	(void)log; (void)type; (void)format; (void)args; (void)ret_addr;
}
