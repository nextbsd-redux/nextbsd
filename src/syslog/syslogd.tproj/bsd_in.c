/*
 * bsd_in.c — FreeBSD-only input module for Apple syslogd.
 *
 * Apple's syslogd never had a UNIX-socket input module: macOS apps
 * use the ASL client library (libsystem_asl) which sends messages
 * over Mach IPC. FreeBSD's existing userland (every program that
 * calls libc's syslog(3)) writes RFC3164 datagrams to /var/run/log
 * (world-writable) and /var/run/logpriv (root-only).
 *
 * This module binds those two sockets and feeds each datagram into
 * asl_input_parse(), the same entry point klog_in / udp_in use.
 * That makes Apple's syslogd a drop-in replacement for FreeBSD's
 * /usr/sbin/syslogd for plain BSD syslog(3) clients.
 *
 * Phase J3 of the ASL port: pkgdemon.github.io/freebsd-asl-plan.html
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include "daemon.h"

#define MY_ID		"bsd_in"
#define BSD_SOCK_PATH	"/var/run/log"
#define BSD_PRIV_PATH	"/var/run/logpriv"
#define MAXLINE		4096

#define NSOCK 2
static int sockfd[NSOCK] = { -1, -1 };
static dispatch_source_t in_src[NSOCK];
static dispatch_queue_t in_queue;
static const char *sock_path[NSOCK] = { BSD_SOCK_PATH, BSD_PRIV_PATH };

static char dline[MAXLINE + 1];

/* Polling-thread fallback. libdispatch's DISPATCH_SOURCE_TYPE_READ
 * doesn't wake up for AF_UNIX SOCK_DGRAM message arrivals on
 * FreeBSD in our build — likely a kqueue/EVFILT_READ interaction
 * with our libdispatch port. Use a real pthread with poll(2) on
 * both fds, just like phase_e_libdispatch_mach_recv did for Mach
 * RECV sources. Replace once dispatch's source firing is fixed. */
#include <pthread.h>
#include <poll.h>
static pthread_t bsd_in_poll_thread;
static int bsd_in_poll_started;

static void
bsd_in_acceptmsg(int fd)
{
	struct sockaddr_un from;
	socklen_t fromlen = sizeof(from);
	ssize_t len;
	asl_msg_t *m;

	/* Phase J runtime debug: log every dispatch fire, BEFORE recvfrom. */
	{ FILE *_d0 = fopen("/tmp/bsd_in_recv.log", "a");
	  if (_d0) { fprintf(_d0, "[%d] dispatch fired on fd=%d\n", getpid(), fd); fclose(_d0); } }

	len = recvfrom(fd, dline, MAXLINE, 0, (struct sockaddr *)&from, &fromlen);
	{ FILE *_d0 = fopen("/tmp/bsd_in_recv.log", "a");
	  if (_d0) { fprintf(_d0, "[%d]   recvfrom rc=%zd errno=%d\n", getpid(), len, errno); fclose(_d0); } }
	if (len <= 0) return;
	dline[len] = '\0';

	/* trim trailing newline if present (BSD syslog(3) appends '\n') */
	while (len > 0 && (dline[len - 1] == '\n' || dline[len - 1] == '\0')) {
		dline[--len] = '\0';
	}
	if (len == 0) return;

	/* Phase J runtime debug. */
	FILE *_d = fopen("/tmp/bsd_in_recv.log", "a");
	if (_d) { fprintf(_d, "[%d] recv fd=%d len=%zd: %.120s\n", getpid(), fd, len, dline); fclose(_d); }

	m = asl_input_parse(dline, len, NULL, SOURCE_BSD_SOCK);
	_d = fopen("/tmp/bsd_in_recv.log", "a");
	if (_d) { fprintf(_d, "[%d]   asl_input_parse -> %p\n", getpid(), (void*)m); fclose(_d); }
	process_message(m, SOURCE_BSD_SOCK);
	_d = fopen("/tmp/bsd_in_recv.log", "a");
	if (_d) { fprintf(_d, "[%d]   process_message OK\n", getpid()); fclose(_d); }
}

static int
bsd_in_open_one(int idx)
{
	struct sockaddr_un sun;
	int fd;
	mode_t perm;

	(void)unlink(sock_path[idx]);

	fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (fd < 0) {
		asldebug("%s: socket(%s): %s\n", MY_ID, sock_path[idx],
		    strerror(errno));
		return -1;
	}

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	strlcpy(sun.sun_path, sock_path[idx], sizeof(sun.sun_path));

	if (bind(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
		asldebug("%s: bind(%s): %s\n", MY_ID, sock_path[idx],
		    strerror(errno));
		close(fd);
		return -1;
	}

	/* /var/run/log world-writable, /var/run/logpriv root-only */
	perm = (idx == 0) ? 0666 : 0600;
	(void)chmod(sock_path[idx], perm);

	if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
		asldebug("%s: O_NONBLOCK(%s): %s\n", MY_ID, sock_path[idx],
		    strerror(errno));
		close(fd);
		return -1;
	}

	sockfd[idx] = fd;
	/* Polling thread (see bsd_in_poll_loop) handles both fds; no
	 * dispatch source needed. */
	asldebug("%s: listening on %s (fd %d)\n", MY_ID, sock_path[idx], fd);
	return 0;
}

static void *
bsd_in_poll_loop(void *unused)
{
	struct pollfd pfd[NSOCK];
	int loops = 0;
	(void)unused;

	/* Phase J runtime debug: log at thread entry — proves the thread
	 * actually started, separate from pthread_create returning 0. */
	{ FILE *_d = fopen("/tmp/bsd_in_recv.log", "a");
	  if (_d) { fprintf(_d, "[%d] poll thread RUNNING (sockfd[0]=%d sockfd[1]=%d)\n",
	    getpid(), sockfd[0], sockfd[1]); fclose(_d); } }

	for (;;) {
		int i, n = 0;
		for (i = 0; i < NSOCK; i++) {
			if (sockfd[i] < 0) continue;
			pfd[n].fd = sockfd[i];
			pfd[n].events = POLLIN;
			pfd[n].revents = 0;
			n++;
		}
		if (n == 0) { usleep(100000); continue; }
		int pr = poll(pfd, n, 1000);
		/* Heartbeat every loop for first 3 iters, then every 5. */
		loops++;
		if (loops <= 3 || loops % 5 == 0) {
			FILE *_d = fopen("/tmp/bsd_in_recv.log", "a");
			if (_d) { fprintf(_d, "[%d] poll loop tick=%d nfds=%d rc=%d (errno=%d)\n",
			    getpid(), loops, n, pr, errno); fclose(_d); }
		}
		if (pr <= 0) continue;
		for (i = 0; i < n; i++) {
			if (pfd[i].revents & POLLIN) bsd_in_acceptmsg(pfd[i].fd);
		}
	}
	return NULL;
}

int
bsd_in_init(void)
{
	static dispatch_once_t once;
	int i, ok = 0;
	FILE *dbg;

	dbg = fopen("/tmp/bsd_in_init.log", "a");
	if (dbg) { fprintf(dbg, "[%d] bsd_in_init: entered\n", getpid()); fclose(dbg); }

	dispatch_once(&once, ^{
		in_queue = dispatch_queue_create(MY_ID, NULL);
	});

	asldebug("%s: init\n", MY_ID);

	for (i = 0; i < NSOCK; i++) {
		if (sockfd[i] >= 0) { ok++; continue; }
		int rc = bsd_in_open_one(i);
		dbg = fopen("/tmp/bsd_in_init.log", "a");
		if (dbg) {
			fprintf(dbg, "[%d] bsd_in_init: open(%s) rc=%d errno=%d (%s)\n",
			    getpid(), sock_path[i], rc, errno, strerror(errno));
			fclose(dbg);
		}
		if (rc == 0) ok++;
	}

	if (ok > 0 && !bsd_in_poll_started) {
		if (pthread_create(&bsd_in_poll_thread, NULL,
		    bsd_in_poll_loop, NULL) == 0) {
			bsd_in_poll_started = 1;
		}
		dbg = fopen("/tmp/bsd_in_init.log", "a");
		if (dbg) { fprintf(dbg, "[%d] bsd_in_init: poll thread started=%d\n", getpid(), bsd_in_poll_started); fclose(dbg); }
	}

	dbg = fopen("/tmp/bsd_in_init.log", "a");
	if (dbg) { fprintf(dbg, "[%d] bsd_in_init: returning ok=%d\n", getpid(), ok); fclose(dbg); }

	return (ok > 0) ? 0 : -1;
}

int
bsd_in_close(void)
{
	int i, did = 0;

	for (i = 0; i < NSOCK; i++) {
		if (sockfd[i] < 0) continue;
		close(sockfd[i]);
		(void)unlink(sock_path[i]);
		sockfd[i] = -1;
		did++;
	}
	/* Polling thread keeps running across reset; only stops when
	 * syslogd exits. Detaching avoids the cancel/release dance. */
	return (did > 0) ? 0 : 1;
}

int
bsd_in_reset(void)
{
	return 0;
}
