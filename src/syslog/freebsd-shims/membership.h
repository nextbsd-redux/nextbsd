/* membership.h — FreeBSD shim. Apple's Open Directory membership
 * API; libsystem_asl uses mbr_uid_to_uuid() to expand uid → UUID
 * for ASL store ACL groups. FreeBSD has no OpenDirectory; stub
 * returns failure so ASL falls back to plain uid/gid checks
 * (still secure, just no group expansion).
 */
#ifndef _FREEBSD_SHIM_MEMBERSHIP_H_
#define _FREEBSD_SHIM_MEMBERSHIP_H_

#include <sys/types.h>
#include <uuid/uuid.h>
#include <errno.h>

static inline int
mbr_uid_to_uuid(uid_t uid, uuid_t uu)
{
	(void)uid; (void)uu;
	return ENOENT;
}

static inline int
mbr_gid_to_uuid(gid_t gid, uuid_t uu)
{
	(void)gid; (void)uu;
	return ENOENT;
}

static inline int
mbr_uuid_to_id(const uuid_t uu, uid_t *uid, int *type)
{
	(void)uu; (void)uid; (void)type;
	return ENOENT;
}

#define ID_TYPE_UID		0
#define ID_TYPE_GID		1

#endif
