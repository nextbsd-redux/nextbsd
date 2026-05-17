/* dispatch/private.h — FreeBSD shim. Apple's libdispatch private API
 * exposes lower-level scheduling primitives our libdispatch port
 * already provides via the public surface. This header just re-exports
 * the public one — Apple-private dispatch APIs that notifyd actually
 * uses (dispatch_mach_create_4libxpc etc.) are covered by the libxpc
 * + libdispatch builds. */
#ifndef _FREEBSD_SHIM_DISPATCH_PRIVATE_H_
#define _FREEBSD_SHIM_DISPATCH_PRIVATE_H_

#include <dispatch/dispatch.h>

#endif
