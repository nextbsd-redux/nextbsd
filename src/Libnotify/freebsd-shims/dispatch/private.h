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

/* QoS classes for dispatch_queue_attr / dispatch_workloop creation.
 * Apple defines via os/qos.h; we don't have it, so define here.
 * dispatch_qos_class_t is `unsigned int` in our libdispatch
 * (src/libdispatch/dispatch/queue.h:559), so don't re-typedef. */

#define QOS_CLASS_USER_INTERACTIVE	0x21
#define QOS_CLASS_USER_INITIATED	0x19
#define QOS_CLASS_DEFAULT		0x15
#define QOS_CLASS_UTILITY		0x11
#define QOS_CLASS_BACKGROUND		0x09
#define QOS_CLASS_UNSPECIFIED		0x00

/* Stub workloop / dispatch_mach creators. Return NULL/sentinel. */
#define dispatch_workloop_create(label)			NULL
#define dispatch_workloop_create_inactive(label)	NULL
#define dispatch_workloop_set_qos_class(wl, qos)	(void)0
#define dispatch_workloop_set_qos_class_floor(wl, qos, rel) (void)0
#define dispatch_workloop_set_autorelease_frequency(wl, freq) (void)0
#define dispatch_mach_create(label, q, handler)		NULL
#define dispatch_mach_create_4libxpc(label, q, handler)	NULL
#define dispatch_mach_create_f(label, q, ctx, handler)	NULL
#define dispatch_mach_connect(mach, recv, checkin, handler) (void)0
#define dispatch_mach_send(mach, msg, options)		(void)0
#define dispatch_mach_msg_get_msg(msg, sz)		NULL
#define dispatch_mach_msg_create(msg, len, dispose, retmsg) NULL

#endif
