/*
 * config_link_monitor.h — in-process KernelEventMonitor (#168 follow-up, #257).
 *
 * Starts a configd-internal thread that reads the PF_ROUTE socket for
 * RTM_IFINFO link-state changes and publishes
 *   State:/Network/Interface/<ifname>/Link = { Active : <bool> }
 * into configd's store (via a configd session, so the write lands on the serve
 * thread). Replaces the standalone KernelEventMonitor daemon. Call once from
 * main(), after the service is checked in and the port set is built, before
 * entering configd_serve().
 */
#ifndef _CONFIG_LINK_MONITOR_H
#define _CONFIG_LINK_MONITOR_H

void config_link_monitor_start(void);

#endif /* _CONFIG_LINK_MONITOR_H */
