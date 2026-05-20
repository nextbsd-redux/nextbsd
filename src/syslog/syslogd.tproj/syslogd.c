/*
 * Copyright (c) 2004-2012 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#include <TargetConditionals.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mach_time.h>
#include <servers/bootstrap.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/errno.h>
#include <sys/queue.h>
#include <sys/time.h>
#include <sys/un.h>
#include <pthread.h>
#include <dirent.h>
#include <dlfcn.h>
#include <libgen.h>
#include <notify.h>
#include <notify_keys.h>
#include <utmpx.h>
#include <asl_private.h>

#if !TARGET_OS_IPHONE
#include <quarantine.h>
#endif
#include "daemon.h"

#define SERVICE_NAME "com.apple.system.logger"
#define SERVER_STATUS_ERROR -1
#define SERVER_STATUS_INACTIVE 0
#define SERVER_STATUS_ACTIVE 1
#define SERVER_STATUS_ON_DEMAND 2

#define BILLION 1000000000

#define NOTIFY_DELAY 1

extern int _malloc_no_asl_log;

#if TARGET_OS_SIMULATOR
const char *_path_pidfile;
const char *_path_syslogd_log;
#endif

/* global */
struct global_s global;

#if !TARGET_OS_SIMULATOR
/* Input Modules */
int klog_in_init(void);
int klog_in_reset(void);
int klog_in_close(void);
static int activate_klog_in = 1;

/* bsd_in: FreeBSD-only UNIX-socket input (Phase J3). */
int bsd_in_init(void);
int bsd_in_reset(void);
int bsd_in_close(void);
static int activate_bsd_in = 1;
#endif

#if !TARGET_OS_SIMULATOR
int udp_in_init(void);
int udp_in_reset(void);
int udp_in_close(void);
static int activate_udp_in = 1;

/* Output Modules */
int bsd_out_init(void);
int bsd_out_reset(void);
int bsd_out_close(void);
static int activate_bsd_out = 1;
#endif

int asl_action_init(void);
int asl_action_reset(void);
int asl_action_close(void);
static int activate_asl_action = 1;

#if !TARGET_OS_SIMULATOR
/* Interactive Module */
int remote_init(void);
int remote_reset(void);
int remote_close(void);
static int remote_enabled = 0;
#endif

extern void database_server();

/* Phase J runtime debug. */
#include <execinfo.h>
void _phasej_sig(int sig);
void _phasej_sig(int sig)
{
	FILE *f = fopen("/tmp/syslogd_sig.log", "a");
	if (f) {
		fprintf(f, "[%d] caught signal %d\n", getpid(), sig);
		void *bt[64];
		int n = backtrace(bt, 64);
		fprintf(f, "[%d] backtrace (%d frames):\n", getpid(), n);
		char **syms = backtrace_symbols(bt, n);
		if (syms) {
			for (int i = 0; i < n; i++)
				fprintf(f, "[%d]   #%-2d %s\n", getpid(), i, syms[i]);
			free(syms);
		}
		fclose(f);
	}
	if (sig == SIGCHLD || sig == SIGPIPE) return;
	_exit(128 + sig);
}

void _phasej_exit(void);
void _phasej_exit(void)
{
	FILE *f = fopen("/tmp/syslogd_sig.log", "a");
	if (f) { fprintf(f, "[%d] atexit handler — exit() called\n", getpid()); fclose(f); }
}

static void
init_modules()
{
#if !TARGET_OS_SIMULATOR
	module_t *m_klog_in, *m_bsd_in, *m_bsd_out, *m_udp_in, *m_remote;
#endif
	module_t *m_asl;
	int m = 0;

	/* ASL module (configured by /etc/asl.conf) */
	m_asl = (module_t *)calloc(1, sizeof(module_t));
	if (m_asl == NULL)
	{
		asldebug("alloc failed (init_modules asl_action)\n");
		exit(1);
	}

	m_asl->name = "asl_action";
	m_asl->enabled = activate_asl_action;
	m_asl->init = asl_action_init;
	m_asl->reset = asl_action_reset;
	m_asl->close = asl_action_close;

	if (m_asl->enabled) m_asl->init();

#if !TARGET_OS_SIMULATOR
	/* BSD output module (configured by /etc/syslog.conf) */
	m_bsd_out = (module_t *)calloc(1, sizeof(module_t));
	if (m_bsd_out == NULL)
	{
		asldebug("alloc failed (init_modules bsd_out)\n");
		exit(1);
	}

	m_bsd_out->name = "bsd_out";
	m_bsd_out->enabled = activate_bsd_out;
	m_bsd_out->init = bsd_out_init;
	m_bsd_out->reset = bsd_out_reset;
	m_bsd_out->close = bsd_out_close;

	if (m_bsd_out->enabled)
	{
		m_bsd_out->init();
		global.bsd_out_enabled = 1;
	}

	/* kernel input module */
	m_klog_in = (module_t *)calloc(1, sizeof(module_t));
	if (m_klog_in == NULL)
	{
		asldebug("alloc failed (init_modules klog_in)\n");
		exit(1);
	}

	m_klog_in->name = "klog_in";
	m_klog_in->enabled = activate_klog_in;
	m_klog_in->init = klog_in_init;
	m_klog_in->reset = klog_in_reset;
	m_klog_in->close = klog_in_close;

	if (m_klog_in->enabled) m_klog_in->init();

	/* bsd_in: FreeBSD-only UNIX-socket input (/var/run/log,
	 * /var/run/logpriv). Phase J3 of ASL port. */
	m_bsd_in = (module_t *)calloc(1, sizeof(module_t));
	if (m_bsd_in == NULL)
	{
		asldebug("alloc failed (init_modules bsd_in)\n");
		exit(1);
	}

	m_bsd_in->name = "bsd_in";
	m_bsd_in->enabled = activate_bsd_in;
	m_bsd_in->init = bsd_in_init;
	m_bsd_in->reset = bsd_in_reset;
	m_bsd_in->close = bsd_in_close;

	if (m_bsd_in->enabled) m_bsd_in->init();
#endif

#if !TARGET_OS_SIMULATOR
	/* network (syslog protocol) input module */
	m_udp_in = (module_t *)calloc(1, sizeof(module_t));
	if (m_udp_in == NULL)
	{
		asldebug("alloc failed (init_modules udp_in)\n");
		exit(1);
	}

	m_udp_in->name = "udp_in";
	m_udp_in->enabled = activate_udp_in;
	m_udp_in->init = udp_in_init;
	m_udp_in->reset = udp_in_reset;
	m_udp_in->close = udp_in_close;

	if (m_udp_in->enabled) m_udp_in->init();

	/* remote (iOS support) module */
	m_remote = (module_t *)calloc(1, sizeof(module_t));
	if (m_remote == NULL)
	{
		asldebug("alloc failed (init_modules remote)\n");
		exit(1);
	}

	m_remote->name = "remote";
	m_remote->enabled = remote_enabled;
	m_remote->init = remote_init;
	m_remote->reset = remote_reset;
	m_remote->close = remote_close;

	if (m_remote->enabled) m_remote->init();
#endif

	/* save modules in global.module array */
#if TARGET_OS_SIMULATOR
	global.module_count = 1;
#else
	global.module_count = 6;	/* +bsd_in (Phase J3) */
#endif
	global.module = (module_t **)calloc(global.module_count, sizeof(module_t *));
	if (global.module == NULL)
	{
		asldebug("alloc failed (init_modules)\n");
		exit(1);
	}

	global.module[m++] = m_asl;
#if !TARGET_OS_SIMULATOR
	global.module[m++] = m_bsd_out;
	global.module[m++] = m_klog_in;
	global.module[m++] = m_bsd_in;
	global.module[m++] = m_udp_in;
	global.module[m++] = m_remote;
#endif
}

static void
writepid(int *first)
{
	struct stat sb;
	FILE *fp;

	if (first != NULL)
	{
		*first = 1;
		memset(&sb, 0, sizeof(struct stat));
		if (stat(_PATH_PIDFILE, &sb) == 0)
		{
			if (S_ISREG(sb.st_mode)) *first = 0;
		}
	}

	fp = fopen(_PATH_PIDFILE, "w");
	if (fp != NULL)
	{
		fprintf(fp, "%d\n", global.pid);
		fclose(fp);
	}
}

void
launch_config()
{
	launch_data_t tmp, pdict;
	kern_return_t status;

	/* Phase J runtime debug. */
#define _PJ_LC(tag) do { \
	FILE *_pjf = fopen("/tmp/launch_config.log", "a"); \
	if (_pjf) { fprintf(_pjf, "[%d] " tag "\n", getpid()); fclose(_pjf); } \
} while(0)

	_PJ_LC("entry");

	tmp = launch_data_new_string(LAUNCH_KEY_CHECKIN);
	_PJ_LC("after launch_data_new_string");
	global.launch_dict = launch_msg(tmp);
	_PJ_LC("after launch_msg");
	launch_data_free(tmp);
	_PJ_LC("after launch_data_free");

	if (global.launch_dict == NULL)
	{
		_PJ_LC("launch_dict NULL — FreeBSD port: return rather than exit");
		asldebug("%d launchd checkin failed\n", global.pid);
		/* FreeBSD port: don't exit. syslogd can still ingest via
		 * bsd_in / klog_in even without a MachService. The dbserver
		 * Mach loop just won't have a port to listen on, which means
		 * no syslog(1) round-trip via Mach — but that path already
		 * hangs on our launchd-842 + mach.ko anyway. */
		return;
	}

	tmp = launch_data_dict_lookup(global.launch_dict, LAUNCH_JOBKEY_MACHSERVICES);
	if (tmp == NULL)
	{
		_PJ_LC("MACHSERVICES NULL — FreeBSD port: return rather than exit");
		asldebug("%d launchd lookup of LAUNCH_JOBKEY_MACHSERVICES failed\n", global.pid);
		return;
	}

	pdict = launch_data_dict_lookup(tmp, SERVICE_NAME);
	if (pdict == NULL)
	{
		_PJ_LC("SERVICE_NAME NULL — FreeBSD port: return rather than exit");
		asldebug("%d launchd lookup of SERVICE_NAME failed\n", global.pid);
		return;
	}
	_PJ_LC("got SERVICE_NAME pdict");

	global.server_port = launch_data_get_machport(pdict);

	/* port for receiving MACH_NOTIFY_DEAD_NAME notifications */
	status = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &(global.dead_session_port));
	if (status != KERN_SUCCESS)
	{
		asldebug("mach_port_allocate dead_session_port failed: %d", status);
		exit(1);
	}

	status = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &(global.listen_set));
	if (status != KERN_SUCCESS)
	{
		asldebug("mach_port_allocate listen_set failed: %d", status);
		exit(1);
	}

	status = mach_port_move_member(mach_task_self(), global.server_port, global.listen_set);
	if (status != KERN_SUCCESS)
	{
		asldebug("mach_port_move_member server_port failed: %d", status);
		exit(1);
	}

	status = mach_port_move_member(mach_task_self(), global.dead_session_port, global.listen_set);
	if (status != KERN_SUCCESS)
	{
		asldebug("mach_port_move_member dead_session_port failed (%u)", status);
		return;
	}
	_PJ_LC("exit OK");
#undef _PJ_LC
}

void
config_debug(int enable, const char *path)
{
	OSSpinLockLock(&global.lock);

	global.debug = enable;
	free(global.debug_file);
	global.debug_file = NULL;
	if (path != NULL) global.debug_file = strdup(path);

	OSSpinLockUnlock(&global.lock);
}

void
config_data_store(int type, uint32_t file_max, uint32_t memory_max, uint32_t str_memory_max)
{
	pthread_mutex_lock(global.db_lock);

	if (global.dbtype & DB_TYPE_FILE)
	{
		asl_store_close(global.file_db);
		global.file_db = NULL;
	}

	if (global.dbtype & DB_TYPE_MEMORY)
	{
		asl_memory_close(global.memory_db);
		global.memory_db = NULL;
	}

	global.dbtype = type;
	global.db_file_max = file_max;
	global.db_memory_max = memory_max;
	global.db_memory_str_max = str_memory_max;

	pthread_mutex_unlock(global.db_lock);
}

void
write_boot_log(int first)
{
	int mib[2] = {CTL_KERN, KERN_BOOTTIME};
	size_t len;
	asl_msg_t *msg;
	char buf[256];
	struct utmpx utx;

	/* Phase J runtime debug: breadcrumb write_boot_log sub-steps. */
#define _WBL(tag) fprintf(stderr, "[%d] wbl: " tag "\n", getpid())

	_WBL("enter");

	if (first == 0)
	{
		/* syslogd restart */
		msg = asl_msg_new(ASL_TYPE_MSG);
		if (msg == NULL) return;

		asl_msg_set_key_val(msg, ASL_KEY_SENDER, "syslogd");
		asl_msg_set_key_val(msg, ASL_KEY_FACILITY, "daemon");
		asl_msg_set_key_val(msg, ASL_KEY_LEVEL, "Notice");
		asl_msg_set_key_val(msg, ASL_KEY_UID, "0");
		asl_msg_set_key_val(msg, ASL_KEY_GID, "0");
		snprintf(buf, sizeof(buf), "%u", global.pid);
		asl_msg_set_key_val(msg, ASL_KEY_PID, buf);
		asl_msg_set_key_val(msg, ASL_KEY_MSG, "--- syslogd restarted ---");
		process_message(msg, SOURCE_INTERNAL);
		return;
	}

	bzero(&utx, sizeof(utx));
	utx.ut_type = BOOT_TIME;
	utx.ut_pid = 1;

	/* get the boot time */
	len = sizeof(struct timeval);
	if (sysctl(mib, 2, &utx.ut_tv, &len, NULL, 0) < 0)
	{
		gettimeofday(&utx.ut_tv, NULL);
	}

	_WBL("before pututxline");
	pututxline(&utx);
	_WBL("after pututxline");

	msg = asl_msg_new(ASL_TYPE_MSG);
	if (msg == NULL) return;

	asl_msg_set_key_val(msg, ASL_KEY_SENDER, "bootlog");
	asl_msg_set_key_val(msg, ASL_KEY_FACILITY, "com.apple.system.utmpx");
	asl_msg_set_key_val(msg, ASL_KEY_LEVEL, "Notice");
	asl_msg_set_key_val(msg, ASL_KEY_UID, "0");
	asl_msg_set_key_val(msg, ASL_KEY_GID, "0");
	asl_msg_set_key_val(msg, ASL_KEY_PID, "0");
	snprintf(buf, sizeof(buf), "BOOT_TIME %lu %u", (unsigned long)utx.ut_tv.tv_sec, (unsigned int)utx.ut_tv.tv_usec);
	asl_msg_set_key_val(msg, ASL_KEY_MSG, buf);
	asl_msg_set_key_val(msg, "ut_id", "0x00 0x00 0x00 0x00");
	asl_msg_set_key_val(msg, "ut_pid", "1");
	asl_msg_set_key_val(msg, "ut_type", "2");
	snprintf(buf, sizeof(buf), "%lu", (unsigned long)utx.ut_tv.tv_sec);
	asl_msg_set_key_val(msg, ASL_KEY_TIME, buf);
	asl_msg_set_key_val(msg, "ut_tv.tv_sec", buf);
	snprintf(buf, sizeof(buf), "%u", (unsigned int)utx.ut_tv.tv_usec);
	asl_msg_set_key_val(msg, "ut_tv.tv_usec", buf);
	snprintf(buf, sizeof(buf), "%u%s", (unsigned int)utx.ut_tv.tv_usec, (utx.ut_tv.tv_usec == 0) ? "" : "000");
	asl_msg_set_key_val(msg, ASL_KEY_TIME_NSEC, buf);

	_WBL("before process_message");
	process_message(msg, SOURCE_INTERNAL);
	_WBL("after process_message");
#undef _WBL
}

int
main(int argc, const char *argv[])
{
	int32_t i;
	uint64_t master_val;
#if !TARGET_OS_SIMULATOR
	int network_change_token;
#endif
	int quota_file_token, asl_db_token, master_token;
	char tstr[32], *notify_key;
	time_t now;
	int first_syslogd_start = 1;

	/* Phase J runtime debug: breadcrumb startup to stderr (captured
	 * reliably at /var/log/syslogd.stderr; /tmp is unreliable — a
	 * fresh fs is mounted over it mid-boot). Defined up-front so the
	 * pre-init_globals path is traced too. */
#define _PJ_BC(tag) fprintf(stderr, "[%d] PJ: " tag "\n", getpid())

#if TARGET_OS_SIMULATOR
	const char *sim_log_dir = getenv("SIMULATOR_LOG_ROOT");
	const char *sim_resource_dir = getenv("SIMULATOR_SHARED_RESOURCES_DIRECTORY");
	char *p;

	/* assert is evil */
	assert(sim_log_dir && sim_resource_dir);

	asprintf((char **)&_path_syslogd_log, "%s/syslogd.log", sim_log_dir);
	asprintf((char **)&_path_pidfile, "%s/var/run/syslog.pid", sim_resource_dir);

	if (_path_syslogd_log == NULL) _path_syslogd_log = "/tmp/syslogd.log";
	else mkpath_np(sim_log_dir, 0755);

	if (_path_pidfile == NULL)
	{
		_path_pidfile = "/tmp/syslog.pid";
	}
	else
	{
		p = strrchr(_path_pidfile, '/');
		*p = '\0';
		mkpath_np(_path_pidfile, 0755);
		*p = '/';
	}
#endif

	_PJ_BC("main entry");

	/* Set I/O policy */
	setiopolicy_np(IOPOL_TYPE_DISK, IOPOL_SCOPE_PROCESS, IOPOL_PASSIVE);

#if !TARGET_OS_IPHONE
	/* Set Quarantine */
	qtn_proc_t qp = qtn_proc_alloc();
	qtn_proc_set_identifier(qp, "com.apple.syslogd");
	qtn_proc_set_flags(qp, QTN_FLAG_SANDBOX | QTN_FLAG_HARD);
	qtn_proc_apply_to_self(qp);
	qtn_proc_free(qp);
#endif

	_PJ_BC("after qtn_proc setup");

	memset(&global, 0, sizeof(struct global_s));

	global.db_lock = (pthread_mutex_t *)calloc(1, sizeof(pthread_mutex_t));
	pthread_mutex_init(global.db_lock, NULL);

	/*
	 * Create work queue, but suspend until output modules are initialized.
	 */
	_PJ_BC("before dispatch_queue_create work_queue");
	global.work_queue = dispatch_queue_create("Work Queue", NULL);
	_PJ_BC("after dispatch_queue_create");
	dispatch_suspend(global.work_queue);
	_PJ_BC("after dispatch_suspend; before init_globals");

	init_globals();
	_PJ_BC("after init_globals; before argv parse");

#if (TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR)
	remote_enabled = 1;
	activate_bsd_out = 0;
#endif

	/* prevent malloc from calling ASL on error */
	_malloc_no_asl_log = 1;

	/* first pass sets up default configurations */
	for (i = 1; i < argc; i++)
	{
		if (streq(argv[i], "-config"))
		{
			if (((i + 1) < argc) && (argv[i+1][0] != '-'))
			{
				i++;
				if (streq(argv[i], "mac"))
				{
					global.dbtype = DB_TYPE_FILE;
					global.db_file_max = 25600000;
				}
				else if (streq(argv[i], "appletv"))
				{
					global.dbtype = DB_TYPE_FILE;
					global.db_file_max = 10240000;
				}
				else if (streq(argv[i], "iphone"))
				{
#if TARGET_OS_SIMULATOR
					global.dbtype = DB_TYPE_FILE;
					global.db_file_max = 25600000;
#else
					global.dbtype = DB_TYPE_MEMORY;
					remote_enabled = 1;
#endif
				}
			}
		}
	}

	for (i = 1; i < argc; i++)
	{
		if (streq(argv[i], "-d"))
		{
			global.debug = 1;
			if (((i+1) < argc) && (argv[i+1][0] != '-')) global.debug_file = strdup(argv[++i]);
		}
		else if (streq(argv[i], "-db"))
		{
			if (((i + 1) < argc) && (argv[i+1][0] != '-'))
			{
				i++;
				if (streq(argv[i], "file"))
				{
					global.dbtype |= DB_TYPE_FILE;
					if (((i + 1) < argc) && (argv[i+1][0] != '-')) global.db_file_max = atol(argv[++i]);
				}
				else if (streq(argv[i], "memory"))
				{
					global.dbtype |= DB_TYPE_MEMORY;
					if (((i + 1) < argc) && (argv[i+1][0] != '-')) global.db_memory_max = atol(argv[++i]);
				}
			}
		}
		else if (streq(argv[i], "-m"))
		{
			if ((i + 1) < argc) global.mark_time = 60 * atoll(argv[++i]);
		}
		else if (streq(argv[i], "-utmp_ttl"))
		{
			if ((i + 1) < argc) global.utmp_ttl = atol(argv[++i]);
		}
		else if (streq(argv[i], "-mps_limit"))
		{
			if ((i + 1) < argc) global.mps_limit = atol(argv[++i]);
		}
		else if (streq(argv[i], "-dup_delay"))
		{
			if ((i + 1) < argc) global.bsd_max_dup_time = atoll(argv[++i]);
		}
#if !TARGET_OS_SIMULATOR
		else if (streq(argv[i], "-klog_in"))
		{
			if ((i + 1) < argc) activate_klog_in = atoi(argv[++i]);
		}
		else if (streq(argv[i], "-udp_in"))
		{
			if ((i + 1) < argc) activate_udp_in = atoi(argv[++i]);
		}
#endif
		else if (streq(argv[i], "-launchd_in"))
		{
			if ((i + 1) < argc) global.launchd_enabled = atoi(argv[++i]);
		}
#if !TARGET_OS_SIMULATOR
		else if (streq(argv[i], "-bsd_out"))
		{
			if ((i + 1) < argc) activate_bsd_out = atoi(argv[++i]);
		}
		else if (streq(argv[i], "-remote"))
		{
			if ((i + 1) < argc) remote_enabled = atoi(argv[++i]);
		}
#endif
	}

	if (global.dbtype == 0)
	{
		global.dbtype = DB_TYPE_FILE;
		global.db_file_max = 25600000;
	}

	signal(SIGHUP, SIG_IGN);

	/* Phase J runtime debug: catch fatal signals so we know what
	 * killed us. */
	{
		extern void _phasej_sig(int);
		signal(SIGTERM, _phasej_sig);
		signal(SIGQUIT, _phasej_sig);
		signal(SIGINT,  _phasej_sig);
		signal(SIGPIPE, _phasej_sig);
		signal(SIGCHLD, _phasej_sig);
		signal(SIGSEGV, _phasej_sig);
		signal(SIGBUS,  _phasej_sig);
		signal(SIGILL,  _phasej_sig);
		signal(SIGABRT, _phasej_sig);
		signal(SIGFPE,  _phasej_sig);
		signal(SIGSYS,  _phasej_sig);
		extern void _phasej_exit(void);
		atexit(_phasej_exit);
	}

	_PJ_BC("after init_globals + argv parse");
	memset(tstr, 0, sizeof(tstr));
	now = time(NULL);
	ctime_r(&now, tstr);
	tstr[19] = '\0';
	asldebug("\n%s syslogd PID %d starting\n", tstr, global.pid);

	_PJ_BC("before writepid");
	writepid(&first_syslogd_start);

	/*
	 * Log UTMPX boot time record
	 */
	_PJ_BC("before write_boot_log");
	write_boot_log(first_syslogd_start);

	/* default NOTIFY_SYSTEM_MASTER settings */
	_PJ_BC("before notify_register_plain");
	master_val = 0x0;
	notify_register_plain(NOTIFY_SYSTEM_MASTER, &master_token);
	_PJ_BC("after notify_register_plain");
	notify_set_state(master_token, master_val);
	_PJ_BC("after notify_set_state");

	asldebug("reading launch plist\n");
	_PJ_BC("before launch_config");
	launch_config();
	_PJ_BC("after launch_config");
	_PJ_BC("about to call asldebug(\"initializing modules\")");
	asldebug("initializing modules\n");
	_PJ_BC("after asldebug; before init_modules");
	init_modules();
	_PJ_BC("after init_modules");

#if !TARGET_OS_SIMULATOR
	asldebug("setting up network change notification handler\n");

	/* network change notification resets UDP and BSD modules */
	notify_register_dispatch(kNotifySCNetworkChange, &network_change_token, global.work_queue, ^(int x){
		if (activate_udp_in != 0) udp_in_reset();
		if (activate_bsd_out != 0) bsd_out_reset();
	});
#endif

	asldebug("setting up quota notification handler\n");

	notify_key = NULL;
	asprintf(&notify_key, "%s%s", NOTIFY_PATH_SERVICE, NOQUOTA_FILE_PATH);
	if (notify_key != NULL)
	{
		int status;

		status = notify_register_dispatch(notify_key, &quota_file_token, dispatch_get_main_queue(), ^(int t){
			struct stat sb;
			memset(&sb, 0, sizeof(sb));
			if (stat(NOQUOTA_FILE_PATH, &sb) == 0)
			{
				char *str = NULL;
				asprintf(&str, "[Sender syslogd] [Level 2] [PID %u] [Facility syslog] [Message *** MESSAGE QUOTAS DISABLED FOR ALL PROCESSES ***]", global.pid);
				internal_log_message(str);
				free(str);
			}
			else
			{
				char *str = NULL;
				asprintf(&str, "[Sender syslogd] [Level 2] [PID %u] [Facility syslog] [Message *** MESSAGE QUOTAS ENABLED ***]", global.pid);
				internal_log_message(str);
				free(str);
			}
		});

		free(notify_key);
	}

	/* SIGHUP resets all modules */
	global.sig_hup_src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)SIGHUP, 0, dispatch_get_main_queue());
	dispatch_source_set_event_handler(global.sig_hup_src, ^{
		dispatch_async(global.work_queue, ^{
			int i;

			asldebug("SIGHUP reset\n");
			for (i = 0; i < global.module_count; i++)
			{
				if (global.module[i]->enabled != 0) global.module[i]->reset();
			}
		});
	});

	dispatch_resume(global.sig_hup_src);

	/* register for DB notification (posted by dbserver) for performance */
	notify_register_plain(kNotifyASLDBUpdate, &asl_db_token);

	/* timer for MARK facility */
	if (global.mark_time > 0)
	{
		global.mark_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
		dispatch_source_set_event_handler(global.mark_timer, ^{ 
			asl_mark();
		});
		dispatch_source_set_timer(global.mark_timer, dispatch_time(DISPATCH_TIME_NOW, global.mark_time * NSEC_PER_SEC), global.mark_time * NSEC_PER_SEC, 0);
		dispatch_resume(global.mark_timer);
	}

	_PJ_BC("about to dispatch database_server");
	asldebug("starting mach service\n");
	/*
	 * Start mach server
	 * Parks a thread in database_server.  In notifyd, we found that the overhead of
	 * a dispatch source for mach calls was too high, especially on iOS.
	 *
	 * FreeBSD port (Phase J runtime): database_server uses
	 * global.server_port which is MACH_PORT_NULL when launch_config
	 * couldn't reach launchd via MachServices. Skip the dispatch
	 * to avoid Mach RPC into our incomplete mach.ko port. bsd_in /
	 * klog_in still ingest cleanly.
	 */
	if (global.server_port != 0) {
		dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
			database_server();
		});
	}
	_PJ_BC("after dispatch database_server (skipped if no server_port)");

	/* go to work */
	asldebug("starting work queue\n");
	_PJ_BC("before dispatch_resume work_queue");
	dispatch_resume(global.work_queue);

	/* FreeBSD port (Phase J runtime): without database_server's
	 * parked Mach receive thread, our libdispatch's dispatch_main
	 * appears to exit when no sources keep it alive. Park on
	 * sigsuspend so the process stays up; bsd_in's polling thread
	 * (and any future dispatch sources) keep doing work. */
	if (global.server_port == 0) {
		/* FreeBSD port (Phase J runtime iter 52): dispatch_main MUST
		 * run on the main thread (asserts pthread_main_np() else
		 * DISPATCH_CLIENT_CRASH). It calls _dispatch_root_queues_init
		 * (needed for dispatch_async from non-dispatch pthreads to
		 * work) then pthread_exit's the main thread. Our bsd_in recv
		 * pthreads (spawned in init_modules above) keep the process
		 * alive; they can dispatch_async cleanly thereafter. */
		_PJ_BC("server_port=0: calling dispatch_main on main thread");
		dispatch_main();
		/* NOTREACHED — dispatch_main pthread_exit's main thread. */
	}
	_PJ_BC("before dispatch_main (parks forever)");
	dispatch_main();
#undef _PJ_BC

	/* NOTREACHED */
	return 0;
}
