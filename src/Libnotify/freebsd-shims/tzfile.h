/* tzfile.h — FreeBSD shim. Apple ships tzfile.h in /usr/include;
 * FreeBSD only in src tree. pathwatch.c uses only TZDIR.
 * Real path /usr/share/zoneinfo matches FreeBSD's. */
#ifndef _FREEBSD_SHIM_TZFILE_H_
#define _FREEBSD_SHIM_TZFILE_H_

#ifndef TZDIR
#define TZDIR	"/usr/share/zoneinfo"
#endif

#endif
