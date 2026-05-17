/* sys/attr.h — FreeBSD shim for Darwin's getattrlist/setattrlist
 * volume attribute API. aslmanager uses it only to set
 * ATTR_CMN_CRTIME (file creation time) on rotated logs; FreeBSD UFS
 * does not expose a portable create-time write API. Stub the struct
 * and make setattrlist a no-op (the timestamp just won't be set). */
#ifndef _FREEBSD_SHIM_SYS_ATTR_H_
#define _FREEBSD_SHIM_SYS_ATTR_H_

#include <sys/types.h>
#include <stdint.h>

#define ATTR_BIT_MAP_COUNT	5
#define ATTR_CMN_CRTIME		0x00000200

typedef uint32_t attrgroup_t;

struct attrlist {
	uint16_t	bitmapcount;
	uint16_t	reserved;
	attrgroup_t	commonattr;
	attrgroup_t	volattr;
	attrgroup_t	dirattr;
	attrgroup_t	fileattr;
	attrgroup_t	forkattr;
};

static inline int
setattrlist(const char *path, struct attrlist *attrList, void *attrBuf,
    size_t attrBufSize, unsigned long options)
{
	(void)path; (void)attrList; (void)attrBuf;
	(void)attrBufSize; (void)options;
	return 0;
}

#endif
