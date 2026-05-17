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

/* mbr_check_membership — Apple's membership-resolver: is uid X a
 * member of group/uuid Y? FreeBSD has POSIX group enumeration only;
 * the membership service is macOS-only. Return ENOENT-style (-1)
 * so callers fall back to "not a member" safely. */
int
mbr_check_membership(void *user, void *group, int *ismember)
{
	(void)user; (void)group;
	if (ismember) *ismember = 0;
	return -1;
}

/* _NSGet{Argv,Argc,Progname,Environ} — Apple crt_externs. On macOS
 * argv lives in a special segment accessed via these getters; on
 * FreeBSD argv is just argv. libsystem_asl uses _NSGetArgv to pull
 * the process executable path for the Sender field of ASL messages.
 * Provide pointers to static empty fallbacks so callers don't crash;
 * Sender just shows as empty. */
extern char **environ;
static char *_asl_stub_empty_argv[] = { NULL };
static int _asl_stub_argc = 0;
static char *_asl_stub_progname = "";

char ***_NSGetArgv(void) { static char **p = _asl_stub_empty_argv; return &p; }
int    *_NSGetArgc(void) { return &_asl_stub_argc; }
char  **_NSGetProgname(void) { return &_asl_stub_progname; }
char ***_NSGetEnviron(void) { return &environ; }

/* vm_allocate / vm_deallocate — Apple's Mach VM wrappers. libmach
 * doesn't ship mach_vm_*; fall through to mmap/munmap. Stand-alone
 * here so libsystem_asl.so doesn't depend on notifyd_stubs.c
 * (which lives next to libnotify). */
#include <sys/mman.h>

int
vm_allocate(unsigned int task, unsigned long *address,
    unsigned long size, int flags)
{
	(void)task; (void)flags;
	void *p = mmap(NULL, size, PROT_READ|PROT_WRITE,
	    MAP_ANON|MAP_PRIVATE, -1, 0);
	if (p == MAP_FAILED) return 3; /* KERN_NO_SPACE */
	*address = (unsigned long)p;
	return 0; /* KERN_SUCCESS */
}

int
vm_deallocate(unsigned int task, unsigned long address, unsigned long size)
{
	(void)task;
	if (munmap((void *)address, size) != 0) return 5; /* KERN_FAILURE */
	return 0;
}
