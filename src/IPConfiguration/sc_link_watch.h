/*
 * sc_link_watch.h — ipconfigd SCDynamicStore link-state watcher.
 *
 * Replaces hwreg_subscribe (the removed hwregd attach trigger) with a
 * watch on State:/Network/Interface/<if>/Link, which the standalone
 * KernelEventMonitor publishes from PF_ROUTE link-state changes. On a
 * link going Active the watcher invokes the caller's callback so
 * ipconfigd can start DHCP on the freshly-linked interface.
 */
#ifndef SC_LINK_WATCH_H
#define SC_LINK_WATCH_H

#include <stdint.h>

/*
 * Invoked (on a libdispatch worker thread) when an interface's link
 * becomes Active. `ifname` is the BSD interface name; `lease_cap_secs`
 * is forwarded verbatim from sc_link_watch_start. The callback decides
 * whether to DHCP (e.g. skip if already bound) and may block in the
 * DHCP + lease loop.
 */
typedef void (*sc_link_watch_cb)(const char *ifname, uint32_t lease_cap_secs);

/*
 * Open a configd session, watch State:/Network/Interface/<if>/Link,
 * and arrange `cb` to run when any interface links up. Also performs an
 * immediate scan of existing keys so a link that was already Active
 * before the watch registered still fires. Returns 0 on success, -1 if
 * the session or watch could not be established (non-fatal to the
 * caller — DHCP simply won't be link-triggered).
 */
int sc_link_watch_start(sc_link_watch_cb cb, uint32_t lease_cap_secs);

#endif /* SC_LINK_WATCH_H */
