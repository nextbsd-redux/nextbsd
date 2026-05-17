/* sys/kauth.h — FreeBSD shim. Apple's kauth (kernel auth) framework.
 * notifyd uses KAUTH_UID_NONE as a sentinel in some access-control
 * paths. FreeBSD has no kauth; just define the constants. */
#ifndef _FREEBSD_SHIM_SYS_KAUTH_H_
#define _FREEBSD_SHIM_SYS_KAUTH_H_

#include <sys/types.h>

#define KAUTH_UID_NONE		((uid_t)-1)
#define KAUTH_GID_NONE		((gid_t)-1)

#endif
