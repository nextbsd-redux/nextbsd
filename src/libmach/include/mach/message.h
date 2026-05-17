/*
 * mach/message.h — minimal userland API for mach_msg.
 *
 * Just enough surface to call mach_msg() through libmach. Type names
 * match Apple's mach/message.h so code can be written portably; the
 * full set of message-descriptor / kernel-internal types from
 * ravynOS's kernel-side header is intentionally not exposed here.
 */
#ifndef _MACH_MESSAGE_H_
#define _MACH_MESSAGE_H_

#include <stdint.h>		/* uint16_t for mach_msg_port_descriptor_t */
#include <mach/mach_traps.h>	/* mach_port_name_t, MACH_PORT_NULL */
#include <mach/std_types.h>	/* natural_t, integer_t, boolean_t, kern_return_t */
				/* On Apple, <mach/message.h> pulls the
				 * machine word types in transitively; we do
				 * the same so Apple-source consumers (migcom,
				 * MIG stubs, launchd-842) that include only
				 * <mach/message.h> still see natural_t etc.
				 * std_types.h's boolean_t / kern_return_t are
				 * guarded, so the typedefs below this point
				 * become no-ops — left in place for clarity. */

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int mach_msg_bits_t;
typedef unsigned int mach_msg_size_t;
typedef int          mach_msg_id_t;
typedef unsigned int mach_msg_option_t;
typedef mach_msg_option_t mach_msg_options_t;	/* Apple-source alias */
typedef unsigned int mach_msg_timeout_t;
typedef int          mach_msg_return_t;
typedef mach_port_name_t mach_port_t;
typedef unsigned char mach_msg_type_name_t;	/* port-right disposition */

/* mach_msg_type_number_t — element count for typed message data /
 * out-of-line arrays. Apple defines it as natural_t (from
 * <mach/std_types.h>, pulled in above). MIG-generated stubs and
 * migcom's type.c both use it for array-length fields. */
typedef natural_t mach_msg_type_number_t;

/*
 * boolean_t — Apple uses it pervasively in Mach APIs. On macOS it
 * lives in <mach/boolean.h>; we inline the typedef here so any
 * Apple-source consumer that #includes <mach/message.h> picks it
 * up. Matches Apple's `typedef unsigned int boolean_t`.
 */
#ifndef _BOOLEAN_T_DEFINED
#define _BOOLEAN_T_DEFINED
typedef unsigned int boolean_t;
#endif

/*
 * audit_token_t — opaque BSM credential token used by Mach trailers
 * and by libxpc's xpc_connection_set_credentials() /
 * xpc_dictionary_get_audit_token(). Apple defines it in <bsm/audit.h>
 * via <mach/message.h>; we inline the typedef here for the same
 * reason. Internal representation must not be inspected directly;
 * audit_token_to_au32() is the canonical accessor.
 */
#ifndef _AUDIT_TOKEN_T_DEFINED
#define _AUDIT_TOKEN_T_DEFINED
typedef struct {
	unsigned int val[8];
} audit_token_t;
#endif

typedef struct {
	mach_msg_bits_t   msgh_bits;
	mach_msg_size_t   msgh_size;
	mach_port_t       msgh_remote_port;
	mach_port_t       msgh_local_port;
	/*
	 * msgh_voucher_port (modern XNU) and msgh_reserved (older XNU,
	 * pre-voucher) occupy the same 32-bit slot. MIG bootstrap_cmds-138
	 * generates stubs that touch ->msgh_reserved; libdispatch /
	 * libxpc clients use ->msgh_voucher_port. Anonymous union lets
	 * both spellings address the same field.
	 */
	union {
		mach_port_name_t	msgh_voucher_port;
		mach_msg_size_t		msgh_reserved;
	};
	mach_msg_id_t     msgh_id;
} mach_msg_header_t;

/* Options bitmask values (matches sys/mach/message.h). */
#define MACH_MSG_OPTION_NONE	0x00000000
#define MACH_SEND_MSG		0x00000001
#define MACH_RCV_MSG		0x00000002
#define MACH_RCV_LARGE		0x00000004
#define MACH_SEND_TIMEOUT	0x00000010
#define MACH_RCV_TIMEOUT	0x00000100

/*
 * Return codes (mach_msg_return_t). Apple's canonical numbering —
 * send-side errors live in 0x10000xxx, receive-side in 0x10004xxx.
 * Full set in sys/mach/message.h.
 */
#define MACH_MSG_SUCCESS		0x00000000

#define MACH_SEND_IN_PROGRESS		0x10000001
#define MACH_SEND_INVALID_DATA		0x10000002
#define MACH_SEND_INVALID_DEST		0x10000003
#define MACH_SEND_TIMED_OUT		0x10000004
#define MACH_SEND_INVALID_NOTIFY	0x10000005
#define MACH_SEND_INVALID_REPLY		0x10000009
#define MACH_SEND_INVALID_RIGHT		0x1000000a
#define MACH_SEND_INVALID_TYPE		0x1000000f
#define MACH_SEND_MSG_TOO_SMALL		0x10000008
#define MACH_SEND_INTERRUPTED		0x10000007
#define MACH_SEND_INVALID_HEADER	0x10000010

#define MACH_RCV_IN_PROGRESS		0x10004001
#define MACH_RCV_INVALID_NAME		0x10004002
#define MACH_RCV_TIMED_OUT		0x10004003
#define MACH_RCV_TOO_LARGE		0x10004004
#define MACH_RCV_INTERRUPTED		0x10004005
#define MACH_RCV_PORT_CHANGED		0x10004006
#define MACH_RCV_INVALID_NOTIFY		0x10004007
#define MACH_RCV_INVALID_DATA		0x10004008
#define MACH_RCV_PORT_DIED		0x10004009
#define MACH_RCV_HEADER_ERROR		0x1000400b
#define MACH_RCV_BODY_ERROR		0x1000400c
#define MACH_RCV_INVALID_TYPE		0x1000400d

/*
 * MACH_MSG_TYPE_* — port-right disposition values used in msgh_bits and
 * in port-right-transfer calls (mach_port_insert_right, MIG descriptors,
 * etc.). Apple keeps the complete set in <mach/message.h>; we follow
 * suit so Apple-source consumers (migcom, MIG-generated stubs,
 * launchd-842) that #include only <mach/message.h> get all of them.
 * <mach/mach_port.h> #includes us and relies on these definitions
 * rather than re-declaring its own copy.
 *
 * The MOVE/COPY/MAKE family (16..22) are the concrete dispositions;
 * the PORT_* names are the abstract aliases MIG uses in .defs files;
 * POLYMORPHIC is the "disposition determined at runtime" sentinel.
 */
#define MACH_MSG_TYPE_PORT_NONE		0

#define MACH_MSG_TYPE_MOVE_RECEIVE	16
#define MACH_MSG_TYPE_MOVE_SEND		17
#define MACH_MSG_TYPE_MOVE_SEND_ONCE	18
#define MACH_MSG_TYPE_COPY_SEND		19
#define MACH_MSG_TYPE_MAKE_SEND		20
#define MACH_MSG_TYPE_MAKE_SEND_ONCE	21
#define MACH_MSG_TYPE_COPY_RECEIVE	22

#define MACH_MSG_TYPE_PORT_NAME		15
#define MACH_MSG_TYPE_PORT_RECEIVE	MACH_MSG_TYPE_MOVE_RECEIVE
#define MACH_MSG_TYPE_PORT_SEND		MACH_MSG_TYPE_MOVE_SEND
#define MACH_MSG_TYPE_PORT_SEND_ONCE	MACH_MSG_TYPE_MOVE_SEND_ONCE
#define MACH_MSG_TYPE_LAST		22

#define MACH_MSG_TYPE_POLYMORPHIC	((mach_msg_type_name_t)-1)

/*
 * Function-like predicate macros — "is this disposition a port-right
 * of some flavor?". migcom uses MACH_MSG_TYPE_PORT_ANY to classify
 * .defs types; MIG-generated stubs use all three. Without the macro
 * defined, the compiler treats MACH_MSG_TYPE_PORT_ANY(x) as an
 * implicit function call and the link fails with "undefined symbol".
 * Bodies match Apple's <mach/message.h> exactly.
 */
#define MACH_MSG_TYPE_PORT_ANY(x)			\
	(((x) >= MACH_MSG_TYPE_MOVE_RECEIVE) &&		\
	 ((x) <= MACH_MSG_TYPE_MAKE_SEND_ONCE))

#define MACH_MSG_TYPE_PORT_ANY_SEND(x)			\
	(((x) >= MACH_MSG_TYPE_MOVE_SEND) &&		\
	 ((x) <= MACH_MSG_TYPE_MAKE_SEND_ONCE))

#define MACH_MSG_TYPE_PORT_ANY_RIGHT(x)			\
	(((x) >= MACH_MSG_TYPE_MOVE_RECEIVE) &&		\
	 ((x) <= MACH_MSG_TYPE_MOVE_SEND_ONCE))

/*
 * MACH_MSGH_BITS(remote, local) — pack a remote-port and local-port
 * disposition into msgh_bits. Matches sys/mach/message.h.
 */
#define MACH_MSGH_BITS(remote, local) \
	((remote) | ((local) << 8))

/*
 * MACH_MSGH_BITS_REMOTE / _LOCAL / _PORTS / _REPLY — unpack msgh_bits
 * into its disposition fields. Apple-canonical masks.
 */
#define MACH_MSGH_BITS_REMOTE_MASK	0x000000ff
#define MACH_MSGH_BITS_LOCAL_MASK	0x0000ff00
#define MACH_MSGH_BITS_VOUCHER_MASK	0x001f0000

/*
 * MACH_MSGH_BITS_ZERO — used when one end of a Mach message has no
 * port (e.g. fire-and-forget sends where no reply is wanted).
 * libnotify uses it for the local_port slot of empty wakeup messages.
 */
#define MACH_MSGH_BITS_ZERO		0

#define MACH_MSGH_BITS_REMOTE(bits) \
	((bits) & MACH_MSGH_BITS_REMOTE_MASK)
#define MACH_MSGH_BITS_LOCAL(bits) \
	(((bits) & MACH_MSGH_BITS_LOCAL_MASK) >> 8)
#define MACH_MSGH_BITS_VOUCHER(bits) \
	(((bits) & MACH_MSGH_BITS_VOUCHER_MASK) >> 16)
#define MACH_MSGH_BITS_PORTS_MASK \
	(MACH_MSGH_BITS_REMOTE_MASK | MACH_MSGH_BITS_LOCAL_MASK)
#define MACH_MSGH_BITS_PORTS(bits) \
	((bits) & MACH_MSGH_BITS_PORTS_MASK)

/*
 * MACH_MSGH_BITS_REPLY — flip remote/local for a reply message.
 * MIG-server demux generates reply headers with this.
 */
#define MACH_MSGH_BITS_REPLY(bits) \
	(MACH_MSGH_BITS_LOCAL(bits) | (MACH_MSGH_BITS_REMOTE(bits) << 8))

/*
 * round_msg(x) — round x up to the next natural_t boundary. Used by
 * MIG-generated stubs and launchd's own message-sizing code. Apple
 * defines it as a macro in <mach/message.h>; we follow suit so the
 * symbol is resolved at compile time, not link time.
 */
#define round_msg(x) \
	(((mach_msg_size_t)(x) + sizeof(natural_t) - 1) & \
	 ~(sizeof(natural_t) - 1))

/*
 * MACH_MSGH_BITS_COMPLEX — set on msgh_bits to signal that the message
 * body has descriptors (port references / OOL memory) after the header.
 * The kernel translates port descriptors between sender / receiver IPC
 * spaces on this flag.
 */
#define MACH_MSGH_BITS_COMPLEX		0x80000000U

/*
 * Descriptor types (msgh_descriptor->type). PORT_DESCRIPTOR carries a
 * port reference; OOL_* carry out-of-line memory (not used yet).
 */
typedef unsigned char mach_msg_descriptor_type_t;
#define MACH_MSG_PORT_DESCRIPTOR		0
#define MACH_MSG_OOL_DESCRIPTOR			1
#define MACH_MSG_OOL_PORTS_DESCRIPTOR		2
#define MACH_MSG_OOL_VOLATILE_DESCRIPTOR	3

/*
 * mach_msg_copy_options_t — how the kernel transfers OOL memory:
 * PHYSICAL_COPY (eager copy), VIRTUAL_COPY (copy-on-write),
 * ALLOCATE (kernel allocates fresh pages on the receiver side).
 */
typedef unsigned int mach_msg_copy_options_t;
#define MACH_MSG_PHYSICAL_COPY		0
#define MACH_MSG_VIRTUAL_COPY		1
#define MACH_MSG_ALLOCATE		2
#define MACH_MSG_OVERWRITE		3

/*
 * Complex-message layout: header + body + descriptors + payload.
 *
 *   - mach_msg_body_t holds descriptor_count.
 *   - Each descriptor (mach_msg_port_descriptor_t or one of the OOL
 *     variants) follows immediately.
 *   - Payload (user-defined fields) follows the descriptor array.
 *
 * #pragma pack(4) on the descriptor types matches the kernel-side
 * layout in <sys/mach/message.h>; without it 64-bit alignment would
 * insert a 4-byte gap after `name` and break wire compatibility.
 *
 * Wire layout is unambiguously 12 bytes per port descriptor. The
 * kernel-side header uses bitfields (pad2:16 / disposition:8 /
 * type:8) which would also be 4 bytes IF the compiler packs them
 * into a single storage unit — but clang on FreeBSD is happy to
 * give each bitfield its own unsigned-int slot when the underlying
 * types differ, producing a 16-byte struct on the wire. To avoid
 * that ambiguity we encode the same on-wire layout with plain
 * uint16_t / uint8_t fields. Apple-source consumers using
 * .disposition / .type still compile (the types collapse to the
 * same unsigned char on both sides).
 */
#pragma pack(4)

typedef struct {
	mach_msg_size_t		msgh_descriptor_count;
} mach_msg_body_t;

typedef struct {
	mach_port_t			name;		/* 4 */
	mach_msg_size_t			pad1;		/* 4 */
	uint16_t			pad2;		/* 2 */
	mach_msg_type_name_t		disposition;	/* 1 — MACH_MSG_TYPE_* */
	mach_msg_descriptor_type_t	type;		/* 1 — MACH_MSG_PORT_DESCRIPTOR */
} mach_msg_port_descriptor_t;	/* total: 12 bytes */

/*
 * OOL (out-of-line) descriptors — carry memory / port arrays passed
 * by reference rather than inline in the message body. Apple's
 * headers express the deallocate/copy/pad/type/disposition fields as
 * `:8` bitfields; we use plain uint8_t fields for the same reason as
 * mach_msg_port_descriptor_t above — clang's bitfield packing across
 * differently-typed members is not wire-deterministic. LP64 layout
 * (our only target): 8-byte address first, then the 4 single-byte
 * fields, then the 4-byte size/count. Total 16 bytes each.
 */
typedef struct {
	void			*address;	/* 8 */
	uint8_t			deallocate;	/* 1 */
	uint8_t			copy;		/* 1 — mach_msg_copy_options_t */
	uint8_t			pad1;		/* 1 */
	mach_msg_descriptor_type_t type;	/* 1 — MACH_MSG_OOL_DESCRIPTOR */
	mach_msg_size_t		size;		/* 4 */
} mach_msg_ool_descriptor_t;	/* total: 16 bytes */

typedef struct {
	void			*address;	/* 8 */
	uint8_t			deallocate;	/* 1 */
	uint8_t			copy;		/* 1 — mach_msg_copy_options_t */
	mach_msg_type_name_t	disposition;	/* 1 — MACH_MSG_TYPE_* */
	mach_msg_descriptor_type_t type;	/* 1 — MACH_MSG_OOL_PORTS_DESCRIPTOR */
	mach_msg_size_t		count;		/* 4 */
} mach_msg_ool_ports_descriptor_t;	/* total: 16 bytes */

/*
 * mach_msg_descriptor_t — the tagged union a complex message's body
 * is an array of. The `type` byte sits at the same offset in every
 * member, so the receiver can switch on it. All three members are
 * 12 or 16 bytes; the union is 16.
 */
typedef union {
	mach_msg_port_descriptor_t	port;
	mach_msg_ool_descriptor_t	out_of_line;
	mach_msg_ool_ports_descriptor_t	ool_ports;
} mach_msg_descriptor_t;

#pragma pack()

/*
 * Mach trailers. Receivers can request the kernel append trailer
 * elements to the received message via MACH_RCV_TRAILER_TYPE() and
 * MACH_RCV_TRAILER_ELEMENTS() in the option bitmask. The trailer
 * format/element type tuple selects which struct is appended; libxpc
 * uses MACH_RCV_TRAILER_AUDIT to pull credentials off Mach IPC.
 *
 * NB: our kernel does not currently materialize audit trailers — the
 * trailer struct fields will be zero — but Apple-source code that
 * reads them still has to type-check at compile time.
 */
typedef unsigned int mach_msg_trailer_type_t;
typedef unsigned int mach_msg_trailer_size_t;
typedef unsigned int mach_port_seqno_t;
typedef uint64_t     mach_port_context_t;

#define MACH_MSG_TRAILER_FORMAT_0	0

#define MACH_RCV_TRAILER_NULL		0
#define MACH_RCV_TRAILER_SEQNO		1
#define MACH_RCV_TRAILER_SENDER		2
#define MACH_RCV_TRAILER_AUDIT		3
#define MACH_RCV_TRAILER_CTX		4

#define MACH_RCV_TRAILER_TYPE(x)	(((x) & 0xf) << 28)
#define MACH_RCV_TRAILER_ELEMENTS(x)	(((x) & 0xf) << 24)

typedef struct {
	mach_msg_trailer_type_t		msgh_trailer_type;
	mach_msg_trailer_size_t		msgh_trailer_size;
} mach_msg_trailer_t;

typedef struct {
	unsigned int			val[2];
} security_token_t;

/* mach_msg_security_trailer_t — trailer with seqno + sender token but
 * no audit token. mach_msg_format_0_trailer_t is Apple's alias for it;
 * the MIG-generated Mach notification structs (mach/notify.h) embed a
 * mach_msg_format_0_trailer_t at the end of every notification. */
typedef struct {
	mach_msg_trailer_type_t		msgh_trailer_type;
	mach_msg_trailer_size_t		msgh_trailer_size;
	mach_port_seqno_t		msgh_seqno;
	security_token_t		msgh_sender;
} mach_msg_security_trailer_t;

typedef mach_msg_security_trailer_t mach_msg_format_0_trailer_t;

typedef struct {
	mach_msg_trailer_type_t		msgh_trailer_type;
	mach_msg_trailer_size_t		msgh_trailer_size;
	mach_port_seqno_t		msgh_seqno;
	security_token_t		msgh_sender;
	audit_token_t			msgh_audit;
} mach_msg_audit_trailer_t;

/*
 * mach_msg_max_trailer_t — largest fixed trailer shape, used by MIG
 * stubs to size receive buffers. The audit trailer is the largest
 * we ship, so alias it under the canonical name.
 */
typedef mach_msg_audit_trailer_t mach_msg_max_trailer_t;

/* Timeout sentinel for mach_msg(): wait forever. */
#define MACH_MSG_TIMEOUT_NONE		((mach_msg_timeout_t)0)

/*
 * Boolean spellings — Apple Mach code uses TRUE/FALSE liberally; we
 * provide them only if not already defined.
 */
#ifndef TRUE
#define TRUE	1
#endif
#ifndef FALSE
#define FALSE	0
#endif

mach_msg_return_t mach_msg(
    mach_msg_header_t *msg,
    mach_msg_option_t  option,
    mach_msg_size_t    send_size,
    mach_msg_size_t    rcv_size,
    mach_port_name_t   rcv_name,
    mach_msg_timeout_t timeout,
    mach_port_name_t   notify);

/*
 * mach_msg_destroy() — release the port rights and OOL memory
 * referenced by a message that won't be sent. Apple-canonical.
 */
void mach_msg_destroy(mach_msg_header_t *msg);

/*
 * mach_msg_server_once() — MIG runtime helper: receive one message,
 * dispatch it through the demux, send the reply. liblaunch's
 * libvproc.c uses it to service helper-downcall requests.
 */
mach_msg_return_t mach_msg_server_once(
    boolean_t (*demux)(mach_msg_header_t *, mach_msg_header_t *),
    mach_msg_size_t max_size, mach_port_name_t rcv_name,
    mach_msg_option_t options);

/*
 * audit_token_to_au32() — unpack a Mach audit_token_t into its eight
 * named uint32 fields. Lives in <bsm/libbsm.h> on Apple; we declare
 * it next to audit_token_t (here in <mach/message.h>) so libmach,
 * which defines it next to mach_msg_destroy, sees the prototype.
 */
void audit_token_to_au32(audit_token_t atok,
    uint32_t *auidp, uint32_t *euidp, uint32_t *egidp,
    uint32_t *ruidp, uint32_t *rgidp, uint32_t *pidp,
    uint32_t *asidp, uint32_t *tidp);

/*
 * mach_msg_send / mach_msg_receive — Apple's thin convenience wrappers
 * around mach_msg(). Inline so we don't add new exported symbols to
 * libsystem_kernel; matches Apple's xnu/libsyscall implementations.
 */
static __inline mach_msg_return_t
mach_msg_send(mach_msg_header_t *msg)
{
	return mach_msg(msg, MACH_SEND_MSG, msg->msgh_size, 0,
	    MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
}

static __inline mach_msg_return_t
mach_msg_receive(mach_msg_header_t *msg)
{
	return mach_msg(msg, MACH_RCV_MSG, 0, msg->msgh_size,
	    msg->msgh_local_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
}

/*
 * kern_return_t — also lives in <mach/mach_port.h> and
 * <mach/kern_return.h>. message.h still defines it (guarded) because
 * the trailer / message types above are written in terms of plain
 * ints but consumers expect kern_return_t to be visible from
 * <mach/message.h> alone.
 */
#ifndef _KERN_RETURN_T_DEFINED
#define _KERN_RETURN_T_DEFINED
typedef int kern_return_t;
#endif

/*
 * mig_reply_error_t and the MIG_* error codes used to live here as a
 * placeholder (with an `int NDR` stand-in field). They now have their
 * canonical home in <mach/mig_errors.h>, where mig_reply_error_t
 * carries the real `NDR_record_t NDR` field the MIG-generated stubs
 * actually emit. Consumers that pulled mig_reply_error_t in via
 * <mach/message.h> get it via the <mach/mach.h> umbrella, which now
 * includes <mach/mig_errors.h>.
 */

/*
 * Standard XNU empty-message types used by Apple daemons (libnotify
 * libnotify.c:401 uses mach_msg_empty_send_t for fire-and-forget
 * wakeup messages). Header-only message — no body — for occasions
 * when the act of receiving is the entire signal.
 */
typedef struct {
	mach_msg_header_t	header;
} mach_msg_empty_send_t;

typedef struct {
	mach_msg_header_t	header;
	mach_msg_trailer_t	trailer;
} mach_msg_empty_rcv_t;

typedef union {
	mach_msg_empty_send_t	send;
	mach_msg_empty_rcv_t	rcv;
} mach_msg_empty_t;

/*
 * MACH_SEND_NOTIFY: option bit for mach_msg() that requests dead-name
 * / send-once notification setup on the named port. Used by libnotify
 * for fire-and-forget signals that should silently drop if the
 * receiver is gone.
 */
#ifndef MACH_SEND_NOTIFY
#define MACH_SEND_NOTIFY		0x00000004
#endif

/*
 * MIG-generated synchronous-IPC option bits (Apple's mach_msg2
 * extension surface). Used by libnotify's MIG-generated User stubs
 * when waiting for replies on the same thread that sent the request.
 * On FreeBSD we don't have priority-inheriting Mach IPC, so these
 * are advisory — define them at the same numeric values XNU uses
 * and the kernel-side mach.ko trap handler ignores any bits it
 * doesn't implement.
 */
#ifndef MACH_SEND_SYNC_OVERRIDE
#define MACH_SEND_SYNC_OVERRIDE		0x00100000
#endif
#ifndef MACH_SEND_SYNC_USE_THRPRI
#define MACH_SEND_SYNC_USE_THRPRI	0x00400000
#endif
#ifndef MACH_RCV_SYNC_WAIT
#define MACH_RCV_SYNC_WAIT		0x00004000
#endif
#ifndef MACH_RCV_SYNC_PEEK
#define MACH_RCV_SYNC_PEEK		0x00008000
#endif

/*
 * Other Apple Mach IPC option bits used by MIG-generated User stubs.
 * MACH_SEND_PROPAGATE_QOS — propagate sender's QoS class with the
 *   message (priority-inheritance hint).
 * MACH_SEND_FILTER_NONFATAL — port-defense filter mismatch returns
 *   error instead of killing the task. Used by libnotify when sending
 *   to potentially-defended ports.
 * Both are advisory on FreeBSD; kernel-side mach.ko ignores bits it
 * doesn't implement.
 */
#ifndef MACH_SEND_PROPAGATE_QOS
#define MACH_SEND_PROPAGATE_QOS		0x00200000
#endif
#ifndef MACH_SEND_FILTER_NONFATAL
#define MACH_SEND_FILTER_NONFATAL	0x00010000
#endif

/* Apple Mach voucher-related option bits + types. We don't implement
 * vouchers; consumers that try to attach/receive vouchers see no-op
 * behavior. */
#ifndef MACH_RCV_VOUCHER
#define MACH_RCV_VOUCHER	0x00000800
#endif
#ifndef MACH_SEND_NO_BUFFER
#define MACH_SEND_NO_BUFFER	0x00020000
#endif

typedef mach_port_t voucher_mach_msg_state_t;

/* MAX_TRAILER_SIZE — largest possible Mach message trailer size.
 * Apple defines as 116; matches their <mach/message.h>. */
#ifndef MAX_TRAILER_SIZE
#define MAX_TRAILER_SIZE	(sizeof(mach_msg_audit_trailer_t) + 8)
#endif

/*
 * mach_port_options_t — Apple's struct passed to mach_port_construct
 * for port creation with attributes (guard tokens, strict semantics).
 * Used by notifyd to create the bootstrap listener port. Define the
 * struct + the MPO_* flags; we ignore them in the trap implementation
 * for now.
 */
/* mach_port_options_t — Apple's port-creation attribute struct.
 * The nested .mpl field carries the port queue limit. We inline
 * the struct (a single uint32_t) rather than include mach_port.h
 * (which would risk a circular header dep). Layout-compatible with
 * Apple's mach_port_limits_t. */
typedef struct {
	uint32_t	flags;
	struct {
		uint32_t	mpl_qlimit;
	} mpl;
	uint64_t	work_interval_id;
	uint64_t	reserved[2];
} mach_port_options_t;
typedef mach_port_options_t *mach_port_options_ptr_t;

#define MPO_CONTEXT_AS_GUARD		0x0001
#define MPO_QLIMIT			0x0002
#define MPO_TEMPOWNER			0x0004
#define MPO_IMPORTANCE_RECEIVER		0x0008
#define MPO_INSERT_SEND_RIGHT		0x0010
#define MPO_STRICT			0x0020
#define MPO_DENAP_RECEIVER		0x0040
#define MPO_IMMOVABLE_RECEIVE		0x0080
#define MPO_FILTER_MSG			0x0100

/* MACH_NOTIFY_* — Mach notification message IDs (delivered to a
 * watcher port when the watched port reaches certain states).
 * Numeric values match XNU. */
#define MACH_NOTIFY_FIRST		0100
#define MACH_NOTIFY_PORT_DELETED	(MACH_NOTIFY_FIRST + 001)
#define MACH_NOTIFY_SEND_POSSIBLE	(MACH_NOTIFY_FIRST + 002)
#define MACH_NOTIFY_PORT_DESTROYED	(MACH_NOTIFY_FIRST + 005)
#define MACH_NOTIFY_NO_SENDERS		(MACH_NOTIFY_FIRST + 006)
#define MACH_NOTIFY_SEND_ONCE		(MACH_NOTIFY_FIRST + 007)
#define MACH_NOTIFY_DEAD_NAME		(MACH_NOTIFY_FIRST + 010)

#ifdef __cplusplus
}
#endif

#endif /* !_MACH_MESSAGE_H_ */
