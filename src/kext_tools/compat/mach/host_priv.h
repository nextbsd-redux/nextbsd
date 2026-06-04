/* mach/host_priv.h — NextBSD compat: host-priv port type (#182).
 * The kext_request(HOST_PRIV,...) load path is re-backed onto kld; this just
 * lets the type references parse. */
#ifndef _NEXTBSD_COMPAT_MACH_HOST_PRIV_H
#define _NEXTBSD_COMPAT_MACH_HOST_PRIV_H
#include <mach/port.h>
#ifndef _HOST_PRIV_T_DEFINED
#define _HOST_PRIV_T_DEFINED
typedef mach_port_t host_priv_t;
#endif
extern host_priv_t mach_host_self(void);
#endif
