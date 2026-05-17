/* dispatch/private.h — FreeBSD shim. Apple's libdispatch private API
 * exposes lower-level scheduling primitives our libdispatch port
 * already provides via the public surface. This header just re-exports
 * the public one — Apple-private dispatch APIs that notifyd actually
 * uses (dispatch_mach_create_4libxpc etc.) are covered by the libxpc
 * + libdispatch builds. */
#ifndef _FREEBSD_SHIM_DISPATCH_PRIVATE_H_
#define _FREEBSD_SHIM_DISPATCH_PRIVATE_H_

#include <dispatch/dispatch.h>

/* Apple-private dispatch types notifyd / configd / etc. use. Our
 * libdispatch doesn't yet ship these; stub as opaque void* so
 * source compiles. Runtime uses won't work until libdispatch is
 * extended (deferred); notifyd's Mach RPC server loop still works
 * via dispatch_source MACH_RECV that we DO have. */
typedef void * dispatch_mach_t;
typedef void * dispatch_mach_msg_t;
typedef void * dispatch_workloop_t;
typedef long   dispatch_mach_reason_t;

#define DISPATCH_MACH_CONNECTED		1
#define DISPATCH_MACH_MESSAGE_RECEIVED	2
#define DISPATCH_MACH_MESSAGE_SENT	3
#define DISPATCH_MACH_BARRIER_COMPLETED	4
#define DISPATCH_MACH_DISCONNECTED	5

#define DISPATCH_MACH_OPTIONS_INSTALL_HANDLER 0x0001

#endif
