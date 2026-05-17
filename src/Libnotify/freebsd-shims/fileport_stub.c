/*
 * fileport_stub.c — userland stubs for Apple's fileport API.
 * Real impl is a J2 prereq mach.ko trap multiplexer op (per
 * pkgdemon.github.io/freebsd-asl-plan.html §13.3). Until that
 * ships, all three calls return ENOSYS / NULL so consumers
 * (libnotify, libsystem_asl, future notifyd / syslogd / configd)
 * link cleanly and observe degraded fd-passing at runtime.
 *
 * Rolled directly into libnotify (the first consumer to need it)
 * via the Makefile SRCS list. When mach.ko gains the real ops
 * this file can move into libmach and the libnotify build drops it.
 */
#include <mach/mach.h>
#include <sys/fileport.h>
#include <errno.h>

int
fileport_makeport(int fd, mach_port_t *port)
{
	(void)fd;
	if (port) *port = MACH_PORT_NULL;
	errno = ENOSYS;
	return -1;
}

int
fileport_makefd(mach_port_t port)
{
	(void)port;
	errno = ENOSYS;
	return -1;
}

void
fileport_invalidate(mach_port_t port)
{
	(void)port;
}
