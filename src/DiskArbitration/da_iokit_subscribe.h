/*
 * da_iokit_subscribe.h — DiskArbitration kernel-notify subscription (C1.3, #218).
 *
 * Learns of storage-device arrival/removal from the kernel notify channel via
 * libIOKit's IOServiceAddMatchingNotification, which (when /dev/ioregistry is
 * present) registers the recv port via IOREGIOCWATCH on the in-kernel registry
 * (#225) and the kernel pushes a binary ioreg_event_msg per matching event. The
 * proven path libIOKit's iokitnotifyrt round-trip exercises (#241, IOKITNOTIFY-OK).
 *
 * DA registers a MATCH-ALL notification (empty matching dictionary) for both
 * arrival (kIOFirstMatchNotification) and departure (kIOTerminatedNotification),
 * then filters storage device names client-side in the callback with the same
 * is_storage_name() rule hwreg_subscribe uses today (da*, ada*, nvd*, nda*, cd*,
 * mmcsd*). This mirrors how hwreg_subscribe receives all events and tags storage.
 *
 * FALLBACK: this path requires /dev/ioregistry. If it is absent (a kernel image
 * predating K1), da_iokit_subscribe_start() returns -1 WITHOUT logging a failure
 * marker, and the caller falls back to the legacy hwreg_subscribe_start() pub/sub
 * path. hwreg_subscribe.c stays compiled as that fallback (PR7/#218 removes it).
 */
#ifndef DISKARBITRATION_DA_IOKIT_SUBSCRIBE_H
#define DISKARBITRATION_DA_IOKIT_SUBSCRIBE_H

/*
 * Start the kernel-notify storage subscription via libIOKit. Returns 0 when the
 * kernel notify channel is present and both arrival + departure notifications
 * registered (the receive thread + dispatch delivery are up). Returns -1 when
 * the kernel path is unavailable (no /dev/ioregistry) or registration failed —
 * the caller should fall back to hwreg_subscribe_start(). Non-fatal either way:
 * the daemon keeps its Mach service registered regardless.
 */
int da_iokit_subscribe_start(void);

#endif /* DISKARBITRATION_DA_IOKIT_SUBSCRIBE_H */
