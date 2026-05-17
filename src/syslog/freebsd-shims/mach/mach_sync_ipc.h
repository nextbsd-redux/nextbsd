/* mach/mach_sync_ipc.h — FreeBSD shim. Apple's synchronous-IPC
 * helpers, generated MIG client stubs include it. The handful of
 * symbols MIG references (e.g. mach_msg2()) get satisfied via our
 * libmach's mach_msg(); we just need the header to resolve. */
#ifndef _FREEBSD_SHIM_MACH_SYNC_IPC_H_
#define _FREEBSD_SHIM_MACH_SYNC_IPC_H_

#include <mach/mach.h>

#endif
