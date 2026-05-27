/*
 * Stub <libc.h> for the passwd port on freebsd-launchd-mach.
 *
 * Apple's <libc.h> is a catch-all wrapper that pulls in many POSIX
 * headers. FreeBSD has no equivalent. Provide the headers passwd.c
 * actually needs by hand. Picked up via -I passwd on the cc command
 * line.
 */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
