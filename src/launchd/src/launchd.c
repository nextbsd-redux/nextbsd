/*
 * Copyright (c) 2005 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_APACHE_LICENSE_HEADER_START@
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 * @APPLE_APACHE_LICENSE_HEADER_END@
 */

#include "config.h"
#include "launchd.h"

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/ucred.h>
#include <sys/fcntl.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/sysctl.h>
#include <kenv.h>
#include <sys/sockio.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/kern_event.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet6/nd6.h>
#include <ifaddrs.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <paths.h>
#include <pwd.h>
#include <grp.h>
#include <ttyent.h>
#include <dlfcn.h>
#include <dirent.h>
#include <string.h>
#include <setjmp.h>
#include <spawn.h>
#include <sched.h>
#include <pthread.h>
#include <util.h>
#include <os/assumes.h>

#if HAVE_LIBAUDITD
#include <bsm/auditd_lib.h>
#include <bsm/audit_session.h>
#endif

#include "bootstrap.h"
#include "vproc.h"
#include "vproc_priv.h"
#include "vproc_internal.h"
#include "launch.h"
#include "launch_internal.h"
#include "launchd_early.h"

#include "runtime.h"
#include "core.h"
#include "ipc.h"
#include "launchd_plist_scan.h"

#define LAUNCHD_CONF ".launchd.conf"

extern char **environ;

static void pfsystem_callback(void *, struct kevent *);

static kq_callback kqpfsystem_callback = pfsystem_callback;

static void pid1_magic_init(void);

static void testfd_or_openfd(int fd, const char *path, int flags);
static bool get_network_state(void);
static void monitor_networking_state(void);
static void fatal_signal_handler(int sig, siginfo_t *si, void *uap);
static void handle_pid1_crashes_separately(void);
static void do_pid1_crash_diagnosis_mode(const char *msg);
static int basic_fork(void);
static bool do_pid1_crash_diagnosis_mode2(const char *msg);
static void launchd_root_make_writable(void);

static void *update_thread(void *nothing);

static void *crash_addr;
static pid_t crash_pid;

char *_launchd_database_dir;
char *_launchd_log_dir;

bool launchd_shutting_down;
bool network_up;
uid_t launchd_uid;
FILE *launchd_console = NULL;
int32_t launchd_sync_frequency = 30;
bool launchd_trace_enabled = false;

int
main(int argc, char *const *argv)
{
	bool sflag = false;
	int ch;

	/*
	 * Read the loader-set kenv "launchd_trace" once at startup. Set via
	 * `set launchd_trace=1` at the FreeBSD loader prompt (see
	 * tests/boot-test.sh for CI's usage). Survives the kernel→PID-1
	 * handoff. Off by default — kenv() returns -1 / errno set on
	 * missing variable, which leaves launchd_trace_enabled = false.
	 */
	{
		char tbuf[8];
		if (kenv(KENV_GET, "launchd_trace", tbuf, sizeof(tbuf)) > 0 &&
		    tbuf[0] == '1')
			launchd_trace_enabled = true;
	}

	/* This needs to be cleaned up. Currently, we risk tripping assumes() macros
	 * before we've properly set things like launchd's log database paths, the
	 * global launchd label for syslog messages and the like. Luckily, these are
	 * operations that will probably never fail, like test_of_openfd(), the
	 * stuff in launchd_runtime_init() and the stuff in
	 * handle_pid1_crashes_separately().
	 */
	testfd_or_openfd(STDIN_FILENO, _PATH_DEVNULL, O_RDONLY);
	testfd_or_openfd(STDOUT_FILENO, _PATH_DEVNULL, O_WRONLY);
	testfd_or_openfd(STDERR_FILENO, _PATH_DEVNULL, O_WRONLY);

	if (launchd_use_gmalloc) {
		if (!getenv("DYLD_INSERT_LIBRARIES")) {
			setenv("DYLD_INSERT_LIBRARIES", "/usr/lib/libgmalloc.dylib", 1);
			setenv("MALLOC_STRICT_SIZE", "1", 1);
			execv(argv[0], argv);
		} else {
			unsetenv("DYLD_INSERT_LIBRARIES");
			unsetenv("MALLOC_STRICT_SIZE");
		}
	} else if (launchd_malloc_log_stacks) {
		if (!getenv("MallocStackLogging")) {
			setenv("MallocStackLogging", "1", 1);
			execv(argv[0], argv);
		} else {
			unsetenv("MallocStackLogging");
		}
	}

	while ((ch = getopt(argc, argv, "s")) != -1) {
		switch (ch) {
		case 's': sflag = true; break;	/* single user */
		case '?': /* we should do something with the global optopt variable here */
		default:
			fprintf(stderr, "%s: ignoring unknown arguments\n", getprogname());
			break;
		}
	}

	if (getpid() != 1 && getppid() != 1) {
		fprintf(stderr, "%s: This program is not meant to be run directly.\n", getprogname());
		exit(EXIT_FAILURE);
	}

	launchd_runtime_init();

	if (NULL == getenv("PATH")) {
		setenv("PATH", _PATH_STDPATH, 1);
	}

	if (pid1_magic) {
		pid1_magic_init();

		int cfd = -1;
		if ((cfd = open(_PATH_CONSOLE, O_WRONLY | O_NOCTTY)) != -1) {
			_fd(cfd);
			if (!(launchd_console = fdopen(cfd, "w"))) {
				(void)close(cfd);
			}
		}

		char *extra = "";
		if (launchd_osinstaller) {
			extra = " in the OS Installer";
		} else if (sflag) {
			extra = " in single-user mode";
		}

		launchd_syslog(LOG_NOTICE | LOG_CONSOLE, "*** launchd[1] has started up%s. ***", extra);
		if (launchd_use_gmalloc) {
			launchd_syslog(LOG_NOTICE | LOG_CONSOLE, "*** Using libgmalloc. ***");
		}

		if (launchd_verbose_boot) {
			launchd_syslog(LOG_NOTICE | LOG_CONSOLE, "*** Verbose boot, will log to /dev/console. ***");
		}

		if (launchd_shutdown_debugging) {
			launchd_syslog(LOG_NOTICE | LOG_CONSOLE, "*** Shutdown debugging is enabled. ***");
		}

		if (launchd_log_shutdown) {
			launchd_syslog(LOG_NOTICE | LOG_CONSOLE, "*** Shutdown logging is enabled. ***");
		}

		if (launchd_log_perf) {
			launchd_syslog(LOG_NOTICE | LOG_CONSOLE, "*** Performance logging is enabled. ***");
		}

		if (launchd_log_debug) {
			launchd_syslog(LOG_NOTICE | LOG_CONSOLE, "*** Debug logging is enabled. ***");
		}

		handle_pid1_crashes_separately();

		/* Start the update thread.
		 *
		 * <rdar://problem/5039559&6153301>
		 */
		pthread_t t = NULL;
		(void)os_assumes_zero(pthread_create(&t, NULL, update_thread, NULL));
		(void)os_assumes_zero(pthread_detach(t));

		/* PID 1 doesn't have a flat namespace. */
		launchd_flat_mach_namespace = false;
		fflush(launchd_console);
		/* LaunchDaemons scan moved to after jobmgr_init() — see below. */
	} else {
		launchd_uid = getuid();
		launchd_var_available = true;
		if (asprintf(&launchd_label, "com.apple.launchd.peruser.%u", launchd_uid) == 0) {
			launchd_label = "com.apple.launchd.peruser.unknown";
		}

		struct passwd *pwent = getpwuid(launchd_uid);
		if (pwent) {
			launchd_username = strdup(pwent->pw_name);
		} else {
			launchd_username = "(unknown)";
		}

		if (asprintf(&_launchd_database_dir, LAUNCHD_DB_PREFIX "/com.apple.launchd.peruser.%u", launchd_uid) == 0) {
			_launchd_database_dir = "";
		}

		if (asprintf(&_launchd_log_dir, LAUNCHD_LOG_PREFIX "/com.apple.launchd.peruser.%u", launchd_uid) == 0) {
			_launchd_log_dir = "";
		}

		if (launchd_allow_global_dyld_envvars) {
			launchd_syslog(LOG_WARNING, "Per-user launchd will allow DYLD_* environment variables in the global environment.");
		}

		ipc_server_init();
		launchd_log_push();

		auditinfo_addr_t auinfo;
		if (posix_assumes_zero(getaudit_addr(&auinfo, sizeof(auinfo))) != -1) {
			launchd_audit_session = auinfo.ai_asid;
			launchd_syslog(LOG_DEBUG, "Our audit session ID is %i", launchd_audit_session);
		}

		launchd_audit_port = _audit_session_self();

		vproc_transaction_begin(NULL);
		vproc_transaction_end(NULL, NULL);

		launchd_syslog(LOG_DEBUG, "Per-user launchd started (UID/username): %u/%s.", launchd_uid, launchd_username);
	}

	monitor_networking_state();
	jobmgr_init(sflag);

	/*
	 * freebsd-launchd-mach (2026-05-16): in-process LaunchDaemons scan.
	 * MUST come after jobmgr_init() (which sets up root_jobmgr) and
	 * before launchd_runtime() (which never returns). Only runs in
	 * PID-1 mode -- per-user launchd loads its own LaunchAgents path
	 * through a different mechanism.
	 *
	 * Implementation in launchd_plist_scan.c. Parses each plist via
	 * libCoreFoundation, converts to launch_data_t, calls job_import().
	 * Matches Apple launchd's actual boot pattern.
	 */
	if (pid1_magic) {
		/*
		 * The kernel mounts / read-only; make it read-write before
		 * any LaunchDaemon is dispatched (Apple does this in
		 * launchctl's do_potential_fsck() ahead of the scan).
		 */
		launchd_root_make_writable();

		/*
		 * freebsd-launchd-mach boot-readiness floor — before any
		 * LaunchDaemon is dispatched:
		 *
		 *   - sethostname(synth) so getty's first banner doesn't
		 *     cache "Amnesiac" (getty/main.c:202 reads gethostname
		 *     once at startup). hostnamed refines this later via
		 *     its full SCPrefs > DHCP > PTR > mDNS chain.
		 *   - open(/dev/klog) so kernel printf() goes to TOLOG only,
		 *     not /dev/console — stops nd6_dad_timer-style messages
		 *     bleeding into the login prompt. fd intentionally
		 *     leaked; we only need logopen() to flip kern.log_open.
		 *
		 * Both are best-effort; failures log + boot continues.
		 */
		{
			char synth_name[64] = "";
			if (launchd_early_sethostname(synth_name,
			    sizeof(synth_name)) == 0)
				launchd_syslog(LOG_NOTICE | LOG_CONSOLE,
				    "early-init: sethostname('%s')",
				    synth_name);
			else
				launchd_syslog(LOG_WARNING | LOG_CONSOLE,
				    "early-init: sethostname failed: %s",
				    strerror(errno));
			if (launchd_early_open_klog() < 0)
				launchd_syslog(LOG_WARNING | LOG_CONSOLE,
				    "early-init: open(/dev/klog) failed "
				    "(kernel printfs may bleed to console): "
				    "%s", strerror(errno));
			else
				launchd_syslog(LOG_NOTICE | LOG_CONSOLE,
				    "early-init: opened /dev/klog "
				    "(kernel.log_open=1; console quiet)");
		}

		launchd_scan_launchdaemons();
		launchd_syslog(LOG_NOTICE | LOG_CONSOLE,
		    "post-scan: entering launchd_runtime() event loop");
	}

	launchd_runtime_init2();
	launchd_runtime();
}

/*
 * Make the root filesystem read-write before any LaunchDaemon starts.
 *
 * The FreeBSD kernel always mounts / read-only — vfs_mountroot.c forces
 * the 'ro' option and deliberately discards vfs.root.mountfrom.options=rw.
 * Apple's launchd performs the read-write transition itself, in
 * launchctl's do_potential_fsck(), run before the LaunchDaemons scan;
 * this port dropped that bootstrapper. Restore the step here so / is
 * writable before getty / syslogd / etc. are dispatched — no rc.d.
 *
 * Filesystem-agnostic: statfs() reports the mounted type and fsck(8) /
 * mount(8) dispatch on it. Works for UFS today; a future root on any
 * filesystem supporting the standard MNT_UPDATE read-only -> read-write
 * remount needs no change here.
 */
static int
launchd_run_tool(const char *const argv[])
{
	pid_t pid;
	int status;
	int pfd[2] = { -1, -1 };
	char errbuf[512];
	ssize_t off = 0, n;

	/* DIAG: capture the tool's stderr (the early console is quiet, so a
	 * failing mount(8)/fsck(8) error would otherwise be lost). Logged
	 * below only on non-zero exit. */
	if (pipe(pfd) == -1) {
		pfd[0] = pfd[1] = -1;
	}
	pid = fork();
	if (pid == -1) {
		if (pfd[0] >= 0) { close(pfd[0]); close(pfd[1]); }
		return -1;
	}
	if (pid == 0) {
		if (pfd[1] >= 0) { dup2(pfd[1], STDERR_FILENO); close(pfd[0]); close(pfd[1]); }
		execv(argv[0], (char *const *)argv);
		_exit(127);
	}
	if (pfd[1] >= 0) close(pfd[1]);
	if (pfd[0] >= 0) {
		while (off < (ssize_t)sizeof(errbuf) - 1 &&
		    (n = read(pfd[0], errbuf + off, sizeof(errbuf) - 1 - off)) > 0) {
			off += n;
		}
		close(pfd[0]);
	}
	errbuf[off] = '\0';
	while (waitpid(pid, &status, 0) == -1) {
		if (errno != EINTR) {
			return -1;
		}
	}
	status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	if (status != 0 && errbuf[0] != '\0') {
		launchd_syslog(LOG_ERR | LOG_CONSOLE,
		    "root-rw: %s stderr: %.400s", argv[0], errbuf);
	}
	return status;
}

static void
launchd_root_make_writable(void)
{
	const char *fsck_argv[]  = { "/sbin/fsck",  "-p",  "/", NULL };
	const char *mount_argv[] = { "/sbin/mount", "-uw", "/", NULL };
	struct statfs sfs;
	int rc;

	if (statfs("/", &sfs) != 0) {
		launchd_syslog(LOG_ERR | LOG_CONSOLE,
		    "root-rw: statfs(/) failed: %s", strerror(errno));
		return;
	}
	if (!(sfs.f_flags & MNT_RDONLY)) {
		return;		/* already read-write — nothing to do */
	}

	/* / is mounted read-only, so running fsck(8) on it is safe. */
	rc = launchd_run_tool(fsck_argv);
	if (rc != 0) {
		launchd_syslog(LOG_WARNING | LOG_CONSOLE,
		    "root-rw: fsck -p / exited %d", rc);
	}

	rc = launchd_run_tool(mount_argv);
	if (rc != 0) {
		launchd_syslog(LOG_ERR | LOG_CONSOLE,
		    "root-rw: mount -uw / exited %d -- / left read-only", rc);
	} else {
		launchd_syslog(LOG_NOTICE | LOG_CONSOLE,
		    "root-rw: / remounted read-write");
	}
}

void
handle_pid1_crashes_separately(void)
{
	struct sigaction fsa;

	fsa.sa_sigaction = fatal_signal_handler;
	fsa.sa_flags = SA_SIGINFO;
	sigemptyset(&fsa.sa_mask);

	(void)posix_assumes_zero(sigaction(SIGILL, &fsa, NULL));
	(void)posix_assumes_zero(sigaction(SIGFPE, &fsa, NULL));
	(void)posix_assumes_zero(sigaction(SIGBUS, &fsa, NULL));
	(void)posix_assumes_zero(sigaction(SIGTRAP, &fsa, NULL));
	(void)posix_assumes_zero(sigaction(SIGABRT, &fsa, NULL));
	(void)posix_assumes_zero(sigaction(SIGSEGV, &fsa, NULL));
}

void *
update_thread(void *nothing __attribute__((unused)))
{
	(void)posix_assumes_zero(setiopolicy_np(IOPOL_TYPE_DISK, IOPOL_SCOPE_THREAD, IOPOL_THROTTLE));

	while (launchd_sync_frequency) {
		sync();
		sleep(launchd_sync_frequency);
	}

	launchd_syslog(LOG_DEBUG, "Update thread exiting.");
	return NULL;
}

#define PID1_CRASH_LOGFILE "/var/log/launchd-pid1.crash"

/* This hack forces the dynamic linker to resolve these symbols ASAP */
static __attribute__((unused)) typeof(sync) *__junk_dyld_trick1 = sync;
static __attribute__((unused)) typeof(sleep) *__junk_dyld_trick2 = sleep;
static __attribute__((unused)) typeof(reboot) *__junk_dyld_trick3 = reboot;

void
do_pid1_crash_diagnosis_mode(const char *msg)
{
	if (launchd_wsp) {
		kill(launchd_wsp, SIGKILL);
		sleep(3);
		launchd_wsp = 0;
	}

	while (launchd_shutdown_debugging && !do_pid1_crash_diagnosis_mode2(msg)) {
		sleep(1);
	}
}

int
basic_fork(void)
{
	int wstatus = 0;
	pid_t p;

	switch ((p = fork())) {
	case -1:
		launchd_syslog(LOG_ERR | LOG_CONSOLE, "Can't fork PID 1 copy for crash debugging: %m");
		return p;
	case 0:
		return p;
	default:
		do {
			(void)waitpid(p, &wstatus, 0);
		} while(!WIFEXITED(wstatus));

		fprintf(stdout, "PID 1 copy: exit status: %d\n", WEXITSTATUS(wstatus));

		return 1;
	}

	return -1;
}

bool
do_pid1_crash_diagnosis_mode2(const char *msg)
{
	if (basic_fork() == 0) {
		/* Neuter our bootstrap port so that the shell doesn't try talking to us
		 * while we're blocked waiting on it.
		 */
		if (launchd_console) {
			fflush(launchd_console);
		}

		task_set_bootstrap_port(mach_task_self(), MACH_PORT_NULL);
		if (basic_fork() != 0) {
			if (launchd_console) {
				fflush(launchd_console);
			}

			return true;
		}
	} else {
		return true;
	}

	int fd;
	revoke(_PATH_CONSOLE);
	if ((fd = open(_PATH_CONSOLE, O_RDWR)) == -1) {
		_exit(2);
	}
	if (login_tty(fd) == -1) {
		_exit(3);
	}

	setenv("TERM", "vt100", 1);
	fprintf(stdout, "\n");
	fprintf(stdout, "Entering launchd PID 1 debugging mode...\n");
	fprintf(stdout, "The PID 1 launchd has crashed %s.\n", msg);
	fprintf(stdout, "It has fork(2)ed itself for debugging.\n");
	fprintf(stdout, "To debug the crashing thread of PID 1:\n");
	fprintf(stdout, "    gdb attach %d\n", getppid());
	fprintf(stdout, "To exit this shell and shut down:\n");
	fprintf(stdout, "    kill -9 1\n");
	fprintf(stdout, "A sample of PID 1 has been written to %s\n", PID1_CRASH_LOGFILE);
	fprintf(stdout, "\n");
	fflush(stdout);

	execl(_PATH_BSHELL, "-sh", NULL);
	syslog(LOG_ERR, "can't exec %s for PID 1 crash debugging: %m", _PATH_BSHELL);
	_exit(EXIT_FAILURE);
}

void
fatal_signal_handler(int sig, siginfo_t *si, void *uap __attribute__((unused)))
{
	const char *doom_why = "at instruction";
	char msg[128];
#if 0
	char *sample_args[] = { "/usr/bin/sample", "1", "1", "-file", PID1_CRASH_LOGFILE, NULL };
	pid_t sample_p;
	int wstatus;
#endif

	crash_addr = si->si_addr;
	crash_pid = si->si_pid;
#if 0
	setenv("XPC_SERVICES_UNAVAILABLE", "1", 0);
	unlink(PID1_CRASH_LOGFILE);

	switch ((sample_p = vfork())) {
	case 0:
		execve(sample_args[0], sample_args, environ);
		_exit(EXIT_FAILURE);
		break;
	default:
		waitpid(sample_p, &wstatus, 0);
		break;
	case -1:
		break;
	}
#endif
	switch (sig) {
	default:
	case 0:
		break;
	case SIGBUS:
	case SIGSEGV:
		doom_why = "trying to read/write";
	case SIGILL:
	case SIGFPE:
	case SIGTRAP:
		snprintf(msg, sizeof(msg), "%s: %p (%s sent by PID %u)", doom_why, crash_addr, strsignal(sig), crash_pid);
		sync();
		do_pid1_crash_diagnosis_mode(msg);
		sleep(3);
		reboot(0);
		break;
	}
}

void
pid1_magic_init(void)
{
	launchd_label = "com.apple.launchd";
	launchd_username = "system";

	_launchd_database_dir = LAUNCHD_DB_PREFIX "/com.apple.launchd";
	_launchd_log_dir = LAUNCHD_LOG_PREFIX "/com.apple.launchd";

	(void)posix_assumes_zero(setsid());
	(void)posix_assumes_zero(chdir("/"));
	(void)posix_assumes_zero(setlogin("root"));

#if !TARGET_OS_EMBEDDED
	auditinfo_addr_t auinfo = {
		.ai_termid = { 
			.at_type = AU_IPv4
		},
		.ai_asid = AU_ASSIGN_ASID,
		.ai_auid = AU_DEFAUDITID,
		.ai_flags = AU_SESSION_FLAG_IS_INITIAL,
	};

	if (setaudit_addr(&auinfo, sizeof(auinfo)) == -1) {
		launchd_syslog(LOG_WARNING | LOG_CONSOLE, "Could not set audit session: %d: %s.", errno, strerror(errno));
		_exit(EXIT_FAILURE);
	}

	launchd_audit_session = auinfo.ai_asid;
	launchd_syslog(LOG_DEBUG, "Audit Session ID: %i", launchd_audit_session);

	launchd_audit_port = _audit_session_self();
#endif // !TARGET_OS_EMBEDDED
}

char *
launchd_copy_persistent_store(int type, const char *file)
{
	char *result = NULL;
	if (!file) {
		file = "";
	}

	switch (type) {
	case LAUNCHD_PERSISTENT_STORE_DB:
		(void)asprintf(&result, "%s/%s", _launchd_database_dir, file);
		break;
	case LAUNCHD_PERSISTENT_STORE_LOGS:
		(void)asprintf(&result, "%s/%s", _launchd_log_dir, file);
		break;
	default:
		break;
	}

	return result;
}

int
_fd(int fd)
{
	if (fd >= 0) {
		(void)posix_assumes_zero(fcntl(fd, F_SETFD, 1));
	}
	return fd;
}

void
launchd_shutdown(void)
{
	int64_t now;

	LD_TRACE("[T41-shutdown] called! pid=%d pid1_magic=%d", getpid(), pid1_magic);

	if (launchd_shutting_down) {
		LD_TRACE("[T41-shutdown] already shutting down, returning");
		return;
	}

	runtime_ktrace0(RTKT_LAUNCHD_EXITING);

	launchd_shutting_down = true;
	launchd_log_push();

	now = runtime_get_wall_time();

	char *term_who = pid1_magic ? "System shutdown" : "Per-user launchd termination for ";
	launchd_syslog(LOG_INFO, "%s%s began", term_who, pid1_magic ? "" : launchd_username);

	os_assert(jobmgr_shutdown(root_jobmgr) != NULL);

#if HAVE_LIBAUDITD
	if (pid1_magic) {
		(void)os_assumes_zero(audit_quick_stop());
	}
#endif
}

void
launchd_SessionCreate(void)
{
#if !TARGET_OS_EMBEDDED
	auditinfo_addr_t auinfo = {
		.ai_termid = { .at_type = AU_IPv4 },
		.ai_asid = AU_ASSIGN_ASID,
		.ai_auid = getuid(),
		.ai_flags = 0,
	};
	if (setaudit_addr(&auinfo, sizeof(auinfo)) == 0) {
		char session[16];
		snprintf(session, sizeof(session), "%x", auinfo.ai_asid);
		setenv("SECURITYSESSIONID", session, 1);
	} else {
		launchd_syslog(LOG_WARNING, "Could not set audit session: %d: %s.", errno, strerror(errno));
	}
#endif // !TARGET_OS_EMBEDDED
}

void
testfd_or_openfd(int fd, const char *path, int flags)
{
	int tmpfd;

	if (-1 != (tmpfd = dup(fd))) {
		(void)posix_assumes_zero(runtime_close(tmpfd));
	} else {
		if (-1 == (tmpfd = open(path, flags | O_NOCTTY, DEFFILEMODE))) {
			launchd_syslog(LOG_ERR, "open(\"%s\", ...): %m", path);
		} else if (tmpfd != fd) {
			(void)posix_assumes_zero(dup2(tmpfd, fd));
			(void)posix_assumes_zero(runtime_close(tmpfd));
		}
	}
}

bool
get_network_state(void)
{
	struct ifaddrs *ifa, *ifai;
	bool up = false;
	int r;

	/* Workaround 4978696: getifaddrs() reports false ENOMEM */
	while ((r = getifaddrs(&ifa)) == -1 && errno == ENOMEM) {
		launchd_syslog(LOG_DEBUG, "Worked around bug: 4978696");
		(void)posix_assumes_zero(sched_yield());
	}

	if (posix_assumes_zero(r) == -1) {
		return network_up;
	}

	for (ifai = ifa; ifai; ifai = ifai->ifa_next) {
		if (!(ifai->ifa_flags & IFF_UP)) {
			continue;
		}
		if (ifai->ifa_flags & IFF_LOOPBACK) {
			continue;
		}
		if (ifai->ifa_addr->sa_family != AF_INET && ifai->ifa_addr->sa_family != AF_INET6) {
			continue;
		}
		up = true;
		break;
	}

	freeifaddrs(ifa);

	return up;
}

void
monitor_networking_state(void)
{
	int pfs = _fd(socket(PF_SYSTEM, SOCK_RAW, SYSPROTO_EVENT));
	struct kev_request kev_req;

	network_up = get_network_state();

	if (pfs == -1) {
		(void)os_assumes_zero(errno);
		return;
	}

	memset(&kev_req, 0, sizeof(kev_req));
	kev_req.vendor_code = KEV_VENDOR_APPLE;
	kev_req.kev_class = KEV_NETWORK_CLASS;

	if (posix_assumes_zero(ioctl(pfs, SIOCSKEVFILT, &kev_req)) == -1) {
		runtime_close(pfs);
		return;
	}

	(void)posix_assumes_zero(kevent_mod(pfs, EVFILT_READ, EV_ADD, 0, 0, &kqpfsystem_callback));
}

void
pfsystem_callback(void *obj __attribute__((unused)), struct kevent *kev)
{
	bool new_networking_state;
	char buf[1024];

	(void)posix_assumes_zero(read((int)kev->ident, &buf, sizeof(buf)));

	new_networking_state = get_network_state();

	if (new_networking_state != network_up) {
		network_up = new_networking_state;
		jobmgr_dispatch_all_semaphores(root_jobmgr);
	}
}
