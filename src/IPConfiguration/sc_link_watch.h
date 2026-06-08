/*
 * sc_link_watch.h — ipconfigd SCDynamicStore link-state watcher.
 *
 * Replaces hwreg_subscribe (the removed hwregd attach trigger) with a
 * watch on State:/Network/Interface/<if>/Link, which the standalone
 * KernelEventMonitor publishes from PF_ROUTE link-state changes. The
 * watcher invokes the caller's callback whenever an interface's Link
 * entity appears or changes — carrying the current Active state — so
 * ipconfigd can both admin-up a newly-seen interface (whose link is
 * still down) and start DHCP once the link is Active.
 */
#ifndef SC_LINK_WATCH_H
#define SC_LINK_WATCH_H

#include <stdint.h>

/*
 * Invoked (on a libdispatch worker thread) for every change to a watched
 * interface's Link entity, AND for each one found by the initial scan.
 * `ifname` is the BSD interface name; `active` is non-zero when the link
 * is up (Active:true), zero when it is present but down. `lease_cap_secs`
 * is forwarded verbatim from sc_link_watch_start. The callback decides
 * what to do (e.g. admin-up the interface when down so its link can
 * negotiate; DHCP when up, skipping if already bound) and may block in
 * the DHCP + lease loop. lo0 is filtered by the watcher.
 */
typedef void (*sc_link_watch_cb)(const char *ifname, int active,
	    uint32_t lease_cap_secs);

/*
 * Open a configd session, watch State:/Network/Interface/<if>/Link,
 * and arrange `cb` to run when any interface links up. Also performs an
 * immediate scan of existing keys so a link that was already Active
 * before the watch registered still fires. Returns 0 on success, -1 if
 * the session or watch could not be established (non-fatal to the
 * caller — DHCP simply won't be link-triggered).
 */
int sc_link_watch_start(sc_link_watch_cb cb, uint32_t lease_cap_secs);

/*
 * Synchronously read State:/Network/Interface/<if>/Link {Active} from configd.
 * Returns non-zero if the interface's link is Active. The store *value* is
 * authoritative even when the change *wakeup* was lost — ipconfigd polls this
 * after admin-up to recover a dropped Active notification (#250).
 */
int link_active_in_store(const char *ifname);

#endif /* SC_LINK_WATCH_H */
