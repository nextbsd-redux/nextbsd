/* crt_externs.h — FreeBSD shim. Apple's accessor for the process's
 * environ / argv / argc pointers. ravynOS already adds these to
 * its libc/gen/crt_externs.c; for our shim, just declare them and
 * link will resolve from libc if available, otherwise provide our
 * own implementation in the consumer .so. */
#ifndef _FREEBSD_SHIM_CRT_EXTERNS_H_
#define _FREEBSD_SHIM_CRT_EXTERNS_H_

#include <unistd.h>

extern char ***_NSGetEnviron(void);
extern char ***_NSGetArgv(void);
extern int *_NSGetArgc(void);
extern char **_NSGetProgname(void);

#endif
