/*
 * lease_loop.h — post-BOUND lease management for ipconfigd.
 *
 * iter 4 owns the BOUND lifecycle:
 *   BOUND      -> sleep until T1 (lease/2)
 *   T1 fires   -> RENEWING: dhcp_renew(); on success re-publish + reset
 *                            timers; on fail, sleep to T2
 *   T2 fires   -> REBINDING: dhcp_renew() (same broadcast path);
 *                            on success re-publish + reset; on fail,
 *                            sleep until lease expiry, then unpublish
 *                            and exit (caller may restart from INIT)
 *
 * Runs on the daemon's main thread — sleeps in 1s ticks so SIGTERM
 * shortens the wait. iter 5 moves this to a per-interface worker
 * alongside the MIG receive loop.
 */
#ifndef _IPCFG_LEASE_LOOP_H_
#define _IPCFG_LEASE_LOOP_H_

#include "dhcp_packet.h"

struct sc_publish;

/*
 * Run the BOUND lease loop for `ifname` until lease expiry or
 * SIGTERM. `lease` is the freshly-bound lease (updated in-place
 * on every successful renewal). `pub` is the (optional) configd
 * publish session — re-publish after each renewal; NULL means no
 * publishing.
 *
 * Returns 0 on clean shutdown (signal), -1 on lease loss.
 */
int	lease_loop_run(const char *ifname, struct dhcp_lease *lease,
	    struct sc_publish *pub);

#endif /* _IPCFG_LEASE_LOOP_H_ */
