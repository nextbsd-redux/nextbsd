/*
 * mach_service.h — ipconfigd's Mach service thread.
 *
 * Owns the receive side of com.apple.IPConfiguration: spawns a
 * worker pthread that bootstrap_check_in's the service port, then
 * runs a raw mach_msg(MACH_RCV_MSG ...) demux loop calling
 * _ipconfig_server() (the MIG-generated demux for ipconfig.defs)
 * on each request. Configd / hwregd use the same pattern.
 *
 * The worker is started up-front so the service is reachable
 * before DHCP completes; routine bodies (in ipconfigd_mig.c)
 * consult bound_state.{c,h} for live lease data and return
 * ipconfig_status_no_server_e until a BOUND lease lands.
 *
 * iter 5b moves per-interface DHCP onto worker threads alongside
 * this one.
 */
#ifndef _IPCFG_MACH_SERVICE_H_
#define _IPCFG_MACH_SERVICE_H_

/*
 * Start the Mach service thread. The thread bootstrap_check_in's
 * the service port itself (so the main thread doesn't have to
 * juggle the receive right) and runs until got_term is set. Safe
 * to call once at daemon startup. Returns 0 on success.
 */
int	mach_service_start(void);

/* Best-effort: wait for the worker to exit (used at shutdown). */
void	mach_service_join(void);

#endif /* _IPCFG_MACH_SERVICE_H_ */
