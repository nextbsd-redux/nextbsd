/*
 * hwreg_mig_types.h — C typedefs for the named MIG types in hwreg.defs.
 *
 * MIG records the wire layout of a `type` declaration but does not
 * emit a C typedef for it: the generated stubs use the type name as-is
 * and expect an imported header to define it. hwreg.defs imports this
 * header so both the generated server (hwregServer.{c,h}) and user
 * (hwregUser.c / hwreg.h) sides see the types. The bounds must match
 * the array/string sizes in hwreg.defs (and the hw_node fields in
 * hwreg_registry.h).
 */
#ifndef HWREG_MIG_TYPES_H
#define HWREG_MIG_TYPES_H

#include <stdint.h>
#include <mach/message.h>

typedef uint64_t	hwreg_id_array_t[128];	/* array[*:128] of uint64_t */
typedef char		hwreg_name_t[32];	/* c_string[32]  */
typedef char		hwreg_path_t[256];	/* c_string[256] */
typedef uint8_t		hwreg_blob_t[2048];	/* array[*:2048] of hwreg_byte_t */

/* hwreg_watch event_mask bits (iter 3). */
#define HWREG_EVT_ARRIVED	0x1u	/* a matching device attached */
#define HWREG_EVT_DEPARTED	0x2u	/* a matching device detached */
#define HWREG_EVT_CHANGED	0x4u	/* a matching device's state changed */

/*
 * hwregd ↔ subscriber/watcher hand-rolled message protocol. The
 * msgh_id range is outside the MIG hwreg subsystem (30000–30009)
 * so the hwregd receive loop can demux MIG calls vs. these. Shared
 * by hwregd (sender) and libIOKit (the IONotificationPort receive
 * thread; see src/libIOKit/IOKitNotify.c).
 */
#define HWREG_MSG_SUBSCRIBE	0x48575201u	/* client -> hwregd: subscribe */
#define HWREG_MSG_EVENT		0x48575202u	/* hwregd -> client: event */

/* sizeof(hwreg_event_msg.text). Must match the struct below. */
#define HWREG_EVENT_TEXT_MAX	479

/*
 * The event message hwregd pushes to a subscriber or to a watch
 * client's notify port. kind = '+' arrived, '-' departed, 'A'
 * subscribe ack; text is a NUL-terminated description
 * (e.g. "arrived id=N name=Y class=Z").
 */
struct hwreg_event_msg {
	mach_msg_header_t	hdr;
	char			kind;
	char			text[HWREG_EVENT_TEXT_MAX];
};

#endif /* HWREG_MIG_TYPES_H */
