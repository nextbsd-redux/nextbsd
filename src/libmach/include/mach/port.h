/*
 * mach/port.h — core Mach port type vocabulary.
 *
 * Apple's <mach/port.h> is the base header defining the port name /
 * port right / port type vocabulary; <mach/mach_port.h> (the
 * port-management RPC layer) includes it. Several Apple-source
 * headers — notably libdispatch's <dispatch/source.h> — pull in
 * <mach/port.h> directly for just the mach_port_t type.
 *
 * Our split keeps mach_port_name_t / MACH_PORT_NULL in
 * <mach/mach_traps.h> and mach_port_t in <mach/message.h>; this
 * header re-exports both and adds the remaining base-vocabulary
 * constants (MACH_PORT_DEAD, the MACH_PORT_TYPE_* set) so a
 * consumer that includes only <mach/port.h> gets the full base
 * surface, matching Apple.
 */
#ifndef _MACH_PORT_H_
#define _MACH_PORT_H_

#include <stdint.h>
#include <mach/mach_traps.h>	/* mach_port_name_t, MACH_PORT_NULL */
#include <mach/message.h>	/* mach_port_t */
#include <mach/mach_port.h>	/* mach_port_right_t, MACH_PORT_RIGHT_* */

#ifndef MACH_PORT_DEAD
#define MACH_PORT_DEAD		((mach_port_name_t)~0)
#endif

#define MACH_PORT_VALID(name) \
	(((name) != MACH_PORT_NULL) && ((name) != MACH_PORT_DEAD))

/*
 * MACH_PORT_INDEX / MACH_PORT_GEN — split a mach_port_name_t into
 * its kernel-table index and per-name generation counter. Apple's
 * canonical layout: low 24 bits are the index, high 8 are the gen.
 * launchd hashes ports by MACH_PORT_INDEX(name).
 */
#define MACH_PORT_INDEX(name)	((name) & ~0xff)
#define MACH_PORT_GEN(name)	(((mach_port_name_t)(name)) & 0xff)
#define MACH_PORT_MAKE(idx, gen) \
	(((mach_port_name_t)(idx) & ~0xff) | ((mach_port_name_t)(gen) & 0xff))

/* mach_port_array_t — an out-of-line array of port names, the shape
 * MIG hands back from RPCs that return port lists (e.g. the launchd
 * job interface's lookup_children). */
typedef mach_port_t *mach_port_array_t;
#ifndef _MACH_PORT_NAME_ARRAY_T_
#define _MACH_PORT_NAME_ARRAY_T_
typedef mach_port_name_t *mach_port_name_array_t;
#endif

/* mach_port_type_t — a bitmask describing which rights a name holds.
 * The MACH_PORT_RIGHT_* values come from <mach/mach_port.h>. */
typedef natural_t mach_port_type_t;
typedef mach_port_type_t *mach_port_type_array_t;

#define MACH_PORT_TYPE(right)		((mach_port_type_t)(1 << ((right) + 16)))
#define MACH_PORT_TYPE_NONE		((mach_port_type_t)0)
#define MACH_PORT_TYPE_SEND		MACH_PORT_TYPE(MACH_PORT_RIGHT_SEND)
#define MACH_PORT_TYPE_RECEIVE		MACH_PORT_TYPE(MACH_PORT_RIGHT_RECEIVE)
#define MACH_PORT_TYPE_SEND_ONCE	MACH_PORT_TYPE(MACH_PORT_RIGHT_SEND_ONCE)
#define MACH_PORT_TYPE_PORT_SET		MACH_PORT_TYPE(MACH_PORT_RIGHT_PORT_SET)
#define MACH_PORT_TYPE_DEAD_NAME	MACH_PORT_TYPE(MACH_PORT_RIGHT_DEAD_NAME)

/*
 * mach_port_type — return the rights bitmask for the named port.
 * libdispatch (event_kevent.c:289) uses it as a coarse name-validity
 * probe. Declared here (not in <mach/mach_traps.h>) because
 * mach_port_type_t lives in this file; declaring it in mach_traps.h
 * would need a circular #include guard that mach_traps.h's prior
 * recursion through port.h breaks.
 */
kern_return_t mach_port_type(mach_port_name_t task, mach_port_name_t name,
    mach_port_type_t *type);

typedef natural_t mach_port_urefs_t;
#ifndef _MACH_PORT_DELTA_T_
#define _MACH_PORT_DELTA_T_
typedef integer_t mach_port_delta_t;
#endif
#ifndef _MACH_PORT_SEQNO_T_
#define _MACH_PORT_SEQNO_T_
typedef natural_t mach_port_seqno_t;
#endif
#ifndef _MACH_PORT_MSCOUNT_T_
#define _MACH_PORT_MSCOUNT_T_
typedef natural_t mach_port_mscount_t;
#endif
#ifndef _MACH_PORT_MSGCOUNT_T_
#define _MACH_PORT_MSGCOUNT_T_
typedef natural_t mach_port_msgcount_t;
#endif
#ifndef _MACH_PORT_RIGHTS_T_
#define _MACH_PORT_RIGHTS_T_
typedef natural_t mach_port_rights_t;
#endif

#endif /* !_MACH_PORT_H_ */
