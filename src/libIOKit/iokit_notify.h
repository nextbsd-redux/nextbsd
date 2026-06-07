/*
 * VENDORED COPY — keep in sync with the kernel canonical.
 *
 * Canonical: nextbsd-kernel src-overlay/sys/sys/mach/iokit_notify.h (installed
 * as <sys/mach/iokit_notify.h> in the kernel sysroot). Userland in this repo
 * cannot include the kernel src-overlay directly, so libIOKit carries a copy of
 * the K-PR2 (#225) device-event Mach message ABI here — the same convention
 * kextd uses for its vendored kernel wire structs (see kextd.c's
 * kextd_load_body_t / src/kext_tools/kextd/iocatalogue.h). This copy keeps ONLY
 * the userland-relevant on-the-wire pieces (the message struct + the msgid +
 * the kind constants); the kernel-only send-side decls under #ifdef _KERNEL in
 * the canonical header are deliberately omitted. The ABI is frozen once
 * shipped, so this mirror should rarely move; if the canonical struct/msgid
 * ever changes, update this file to match.
 *
 * --- canonical contents below (userland-relevant subset), verbatim ---
 *
 * iokit_notify.h — kernel->userland IORegistry device-event Mach message
 * (K-PR2, nextbsd#225, part of #211/#218).
 *
 * The Apple-faithful device-arrival/departure notification channel. A userland
 * client (DiskArbitration, the IOKitNotify migration) creates a Mach receive
 * right and registers it via IOREGIOCWATCH on /dev/ioregistry (see
 * <sys/ioregistry.h>) together with a packed-nvlist match bag and an event
 * mask. The kernel copies a send right to that port and, from the newbus
 * device_attach / device_detach eventhandlers, pushes one ioreg_event_msg per
 * matching event to the client's port.
 */
#ifndef _SYS_MACH_IOKIT_NOTIFY_H_
#define _SYS_MACH_IOKIT_NOTIFY_H_

#include <mach/message.h>
#include <mach/ndr.h>

/*
 * msgh_id for a device-event notification — "IONT", chosen outside the
 * MACH_NOTIFY_* range (0x0100..) and distinct from IOKIT_KEXTD_LOAD_MSGID
 * ("IOKT", 0x494f4b54). The userland receivers (the PR4/PR5 IOKitNotify /
 * DiskArbitration migration) MUST match on this id.
 */
#define	IOKIT_NOTIFY_EVENT_MSGID	0x494f4e54	/* 'I','O','N','T' */

/* Event kinds carried in ioreg_event_msg.kind — value-identical to the
 * IOREG_EVENT_* mask bits in <sys/ioregistry.h>. Re-stated here so a userland
 * receiver of this message need only include this one header. */
#define	IOKIT_NOTIFY_KIND_ARRIVE	0x00000001	/* device attached */
#define	IOKIT_NOTIFY_KIND_DEPART	0x00000002	/* device detached */
#define	IOKIT_NOTIFY_KIND_MATCHED	0x00000004	/* device bound driver */

#define	IOKIT_NOTIFY_NAME_MAX		32		/* == IOREG_NAME_MAX */

/*
 * Wire format. Fixed-size, inline (no out-of-line/complex descriptors) so the
 * kernel send is trivial and the client reads it with a plain mach_msg. The
 * body identifies the device by its stable registry id (see <sys/ioregistry.h>)
 * plus its newbus name/class and PCI ids (0 if not a PCI nub), enough for a
 * client to act or to issue follow-up IOREGIOC* queries.
 */
typedef struct {
	mach_msg_header_t		hdr;
	NDR_record_t			NDR;
	uint32_t			kind;		/* IOKIT_NOTIFY_KIND_* */
	uint32_t			_pad;		/* keep id 8-byte aligned */
	uint64_t			id;		/* stable registry node id */
	char				name[IOKIT_NOTIFY_NAME_MAX];	/* device_get_name() */
	char				classname[IOKIT_NOTIFY_NAME_MAX]; /* devclass name */
	uint32_t			pci_vendor;	/* 0 if not a PCI nub */
	uint32_t			pci_device;	/* 0 if not a PCI nub */
	mach_msg_format_0_trailer_t	trailer;	/* appended on receive */
} ioreg_event_msg;

#endif /* _SYS_MACH_IOKIT_NOTIFY_H_ */
