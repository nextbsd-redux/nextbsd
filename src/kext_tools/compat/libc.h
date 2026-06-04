/* libc.h — NextBSD compat: macOS umbrella header. FreeBSD has no <libc.h>;
 * pull in the common C headers the Apple sources expect from it. (#182) */
#ifndef _NEXTBSD_COMPAT_LIBC_H
#define _NEXTBSD_COMPAT_LIBC_H
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#endif
