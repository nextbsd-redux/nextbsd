/* os/object_private.h — FreeBSD shim. Apple's libsystem object
 * reference-counting framework — the basis of dispatch_*, xpc_*,
 * os_log_t, asl_object_t, etc. on macOS. Internally uses runtime
 * support from libdispatch and libobjc.
 *
 * For our port: provide enough type/macro support so the headers
 * that mention OS_OBJECT_* compile. Concrete object impls (asl,
 * xpc, etc.) provide their own retain/release semantics.
 */
#ifndef _FREEBSD_SHIM_OS_OBJECT_PRIVATE_H_
#define _FREEBSD_SHIM_OS_OBJECT_PRIVATE_H_

#include <os/base.h>

/* Apple uses these to declare opaque types and their refcount API.
 * For our stub: just typedef the class to void* so casts work. */
#define OS_OBJECT_DECL_CLASS(name)		typedef void * name##_t
#define OS_OBJECT_DECL_SUBCLASS(name, super)	typedef void * name##_t
#define OS_OBJECT_DECL_IMPL_CLASS(name)		typedef void * name##_t

#define _OS_OBJECT_DECL_PROTOCOL(name, ...)
#define OS_OBJECT_RETURNS_RETAINED
#define OS_OBJECT_RELEASES_ARGUMENT

#endif
