/*
 * xpc/private.h — XPC private/SPI surface for launchd-842.
 *
 * On Apple, <xpc/private.h> is the SPI half of libxpc — the bits not
 * in the public <xpc/xpc.h>. launchd-842's core.h includes it.
 *
 * Almost everything launchd reaches for under the "xpc-private"
 * banner — the xpc_domain_* / xpc_event_* / xpc_process_* /
 * _xpc_domain_* function families — is *defined inside launchd's own
 * core.c*, not provided by libxpc. They're launchd's MIG-handler
 * implementations, forward-declared file-locally. So this header
 * does NOT declare them.
 *
 * What core.c actually needs from xpc-private and isn't already in
 * libxpc's public xpc.h:
 *   - launchd's internal XPC protocol vocabulary (routine opcodes,
 *     dictionary key strings, the priority-band enum). These are
 *     launchd-internal: no other consumer; values don't have to
 *     match Apple, only stay consistent between launchd + launchctl.
 *   - the xpc_service_type_t / xpc_jetsam_band_t typedefs.
 *
 * Phase I1c uses these only at compile time — the XPC IPC paths are
 * not exercised by the no-IPC CLI smoke. Values are placeholder.
 */
#ifndef _XPC_PRIVATE_H_
#define _XPC_PRIVATE_H_

#include <stdint.h>
#include <mach/mach.h>		/* mach_port_t for xpc_dictionary_* hooks */

#include <xpc/xpc.h>		/* xpc_object_t + the base XPC API */
#include <xpc/launchd.h>	/* xpc_pipe_*, xpc_dictionary_get_audit_token, ... */

/* ---- private type vocabulary ----------------------------------- */

typedef int64_t  xpc_jetsam_band_t;
typedef uint64_t xpc_service_type_t;

/*
 * xpc_event_publisher_t — opaque handle for the XPC event publisher
 * API (Apple's xpc_event_publisher_create / _send / etc.). libnotify
 * stores one in notify_state_t::event_publisher to fan out
 * notifications to XPC subscribers. We declare the type so libnotify
 * compiles; the actual publisher API is a J2 add when notifyd needs
 * to actually publish XPC events.
 */
typedef void * xpc_event_publisher_t;

/*
 * xpc_event_publisher API — notifyd uses this to fan out events
 * to XPC subscribers. We declare the opaque action enum + stub the
 * setter/sender; runtime use no-ops since we don't have the XPC
 * event substrate yet (J3+ if needed).
 */
typedef enum {
	XPC_EVENT_PUBLISHER_ACTION_ADD = 1,
	XPC_EVENT_PUBLISHER_ACTION_REMOVE = 2,
} xpc_event_publisher_action_t;

#define xpc_event_publisher_create(stream, queue)	NULL
#define xpc_event_publisher_set_handler(pub, handler)	(void)0
#define xpc_event_publisher_fire(pub, name, event)	(void)0
#define xpc_event_publisher_activate(pub)		(void)0
#define xpc_event_publisher_cancel(pub)			(void)0

/*
 * event_name_t — fixed-size char array launchd-842 uses to carry
 * service / event names across the XPC-domain RPCs. Apple's launchd
 * relies on a 128-char buffer; core.c does
 * `strlcpy(name, msi->name, sizeof(event_name_t))` so the array form
 * (not a pointer) is required.
 */
typedef char event_name_t[128];

/*
 * __RequestUnion__xpc_domain_xpc_domain_subsystem +
 * __ReplyUnion__xpc_domain_xpc_domain_subsystem — MIG-generated
 * unions Apple emits from a domain.defs Apple keeps in the SDK
 * (usr/local/include/xpc/domain.defs). We don't ship it; runtime.c
 * references the unions only inside a sizeof() to compute the
 * launchd_runtime_init() max_msg_size buffer. A 4 KiB placeholder
 * payload is more than enough headroom for any XPC-domain message.
 */
union __RequestUnion__xpc_domain_xpc_domain_subsystem {
	char	_xpc_domain_req_pad[4096];
};
union __ReplyUnion__xpc_domain_xpc_domain_subsystem {
	char	_xpc_domain_rep_pad[4096];
};

/*
 * xpc_jetsam_band_t — launchd's view of jetsam priority bands as seen
 * over XPC. Underlies the launchctl `setpriority` plumbing. Apple's
 * exact numbering is not load-bearing here.
 */
enum {
	XPC_JETSAM_BAND_SUSPENDED = 1,
	XPC_JETSAM_BAND_BACKGROUND_OPPORTUNISTIC,
	XPC_JETSAM_BAND_BACKGROUND,
	XPC_JETSAM_BAND_MAIL,
	XPC_JETSAM_BAND_PHONE,
	XPC_JETSAM_BAND_UI_SUPPORT,
	XPC_JETSAM_BAND_FOREGROUND_SUPPORT,
	XPC_JETSAM_BAND_FOREGROUND,
	XPC_JETSAM_BAND_AUDIO,
	XPC_JETSAM_BAND_ACCESSORY,
	XPC_JETSAM_BAND_CRITICAL,
	XPC_JETSAM_BAND_TELEPHONY,
	XPC_JETSAM_BAND_LAST
};

#define XPC_JETSAM_PRIORITY_RESERVED	100

/* ---- domain types ---------------------------------------------- */

#define XPC_DOMAIN_TYPE_SYSTEM		1
#define XPC_DOMAIN_TYPE_PERUSER		2
#define XPC_DOMAIN_TYPE_PERSESSION	3

/* ---- event-routine opcodes + keys ------------------------------ */

#define XPC_EVENT_CHECK_IN		1
#define XPC_EVENT_COPY			2
#define XPC_EVENT_COPY_ENTITLEMENTS	3
#define XPC_EVENT_GET_NAME		4
#define XPC_EVENT_LOOK_UP		5
#define XPC_EVENT_PROVIDER_CHECK_IN	6
#define XPC_EVENT_PROVIDER_SET_STATE	7
#define XPC_EVENT_SET			8

#define XPC_EVENT_FLAG_ALLOW_UNMANAGED	0x1
#define XPC_EVENT_FLAG_ENTITLEMENTS	0x2

#define XPC_EVENT_ROUTINE_KEY_ENTITLEMENTS	"entitlements"
#define XPC_EVENT_ROUTINE_KEY_ERROR		"error"
#define XPC_EVENT_ROUTINE_KEY_EVENT		"event"
#define XPC_EVENT_ROUTINE_KEY_EVENTS		"events"
#define XPC_EVENT_ROUTINE_KEY_FLAGS		"flags"
#define XPC_EVENT_ROUTINE_KEY_NAME		"name"
#define XPC_EVENT_ROUTINE_KEY_OP		"op"
#define XPC_EVENT_ROUTINE_KEY_PORT		"port"
#define XPC_EVENT_ROUTINE_KEY_STATE		"state"
#define XPC_EVENT_ROUTINE_KEY_STREAM		"stream"
#define XPC_EVENT_ROUTINE_KEY_TOKEN		"token"

/* ---- process-routine opcodes + keys ---------------------------- */

#define XPC_PROCESS_JETSAM_SET_BAND		1
#define XPC_PROCESS_JETSAM_SET_MEMORY_LIMIT	2
#define XPC_PROCESS_SERVICE_ATTACH		3
#define XPC_PROCESS_SERVICE_DETACH		4
#define XPC_PROCESS_SERVICE_GET_PROPERTIES	5
#define XPC_PROCESS_SERVICE_KILL		6

#define XPC_PROCESS_ROUTINE_KEY_ARGV		"argv"
#define XPC_PROCESS_ROUTINE_KEY_ERROR		"error"
#define XPC_PROCESS_ROUTINE_KEY_HANDLE		"handle"
#define XPC_PROCESS_ROUTINE_KEY_LABEL		"label"
#define XPC_PROCESS_ROUTINE_KEY_MEMORY_LIMIT	"memory_limit"
#define XPC_PROCESS_ROUTINE_KEY_NAME		"name"
#define XPC_PROCESS_ROUTINE_KEY_NEW_INSTANCE_PORT "new_instance_port"
#define XPC_PROCESS_ROUTINE_KEY_OP		"op"
#define XPC_PROCESS_ROUTINE_KEY_PATH		"path"
#define XPC_PROCESS_ROUTINE_KEY_PID		"pid"
#define XPC_PROCESS_ROUTINE_KEY_PRIORITY_BAND	"priority_band"
#define XPC_PROCESS_ROUTINE_KEY_RCDATA		"rcdata"
#define XPC_PROCESS_ROUTINE_KEY_SIGNAL		"signal"
#define XPC_PROCESS_ROUTINE_KEY_TYPE		"type"

/* ---- service-type vocabulary ----------------------------------- */

#define XPC_SERVICE_TYPE_APP			1
#define XPC_SERVICE_TYPE_BUNDLED		2
#define XPC_SERVICE_TYPE_LAUNCHD		3

#define XPC_SERVICE_ENTITLEMENT_ATTACH		"com.apple.xpc.attach"
#define XPC_SERVICE_ENV_ATTACHED		"XPC_SERVICE_ATTACHED"
#define XPC_SERVICE_RENDEZVOUS_TOKEN		"XPC_SERVICE_RENDEZVOUS_TOKEN"

/* ---- misc launchd-XPC dictionary keys -------------------------- */

#define XPC_DOMAIN				"Domain"
#define XPC_LABEL				"Label"
#define XPC_SINGLETON				"Singleton"
#define XPC_LPI_VERSION				1

#endif /* !_XPC_PRIVATE_H_ */
