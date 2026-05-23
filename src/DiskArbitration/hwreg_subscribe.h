/*
 * hwreg_subscribe.h — DiskArbitration iter 2 hwregd pub/sub client.
 *
 * Mirrors hwregtest.c's pattern: bootstrap_look_up org.freebsd.hwregd,
 * allocate a notify port, send a raw HWREG_MSG_SUBSCRIBE, wait for the
 * ack EVENT, then spawn a pthread that loops on mach_msg(MACH_RCV_MSG)
 * and logs every event hwregd pushes. Storage-looking device names
 * (ada*, da*, nvd*, cd*, mmcsd*) are tagged STORAGE in the log; full
 * registry filtering + initial enumeration are iter 3.
 */
#ifndef DISKARBITRATION_HWREG_SUBSCRIBE_H
#define DISKARBITRATION_HWREG_SUBSCRIBE_H

/*
 * Start the hwregd subscription. Returns 0 on success (subscription
 * established + ack received; receive thread running). Returns -1 on
 * any error path (logged inline). Non-fatal in iter 2 — the daemon
 * still keeps the Mach service registered, just without storage
 * events.
 */
int hwreg_subscribe_start(void);

#endif /* DISKARBITRATION_HWREG_SUBSCRIBE_H */
