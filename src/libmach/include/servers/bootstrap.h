/*
 * servers/bootstrap.h — userland bootstrap-server client API.
 *
 * Matches the Apple-canonical header at the same path. Two operations
 * for now (the subset ravynOS-fork libxpc reaches for):
 *
 *   bootstrap_check_in(bp, name, &port)
 *      Server side. Caller asks the bootstrap server to register
 *      `name` as a service. On success, the caller receives a
 *      receive right (`*port`) to use for the service's incoming
 *      messages. The bootstrap server keeps a send right internally.
 *
 *   bootstrap_look_up(bp, name, &port)
 *      Client side. Caller asks the bootstrap server for the port
 *      registered under `name`. On success, the caller receives a
 *      send right (`*port`).
 *
 * `bp` is the bootstrap port (a send right to the bootstrap server's
 * receive port). Apple stashes this on each task via
 * TASK_BOOTSTRAP_PORT; libxpc reads it via mach_init().
 *
 * On Apple, libbootstrap.c uses MIG-generated client stubs. Our impl
 * uses hand-rolled message-ID dispatch (see src/bootstrap/) since
 * we don't ship MIG.
 *
 * If mach.ko isn't loaded or the bootstrap server isn't reachable,
 * returns the appropriate kern_return_t failure code.
 */
#ifndef _SERVERS_BOOTSTRAP_H_
#define _SERVERS_BOOTSTRAP_H_

#include <mach/mach_traps.h>		/* mach_port_name_t */
#include <mach/mach_port.h>		/* kern_return_t */
#include <mach/message.h>		/* mach_port_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Apple convention: name_t is a fixed-length char array. Match the
 * BOOTSTRAP_MAX_NAME_LEN (128) on the wire. */
typedef char name_t[128];
typedef const char *cmd_t;

/*
 * Process-global bootstrap port. Apple's dyld initializes this before
 * main() from TASK_BOOTSTRAP_PORT. Our libbootstrap does the same via
 * a __attribute__((constructor)) in libbootstrap.c — callers see it
 * already set by the time main() runs. If task_get_bootstrap_port
 * fails (mach.ko not loaded, no bootstrap server running) it stays
 * at MACH_PORT_NULL and bootstrap_check_in / bootstrap_look_up
 * surface that as KERN_FAILURE.
 */
extern mach_port_t bootstrap_port;

/*
 * Apple-canonical bootstrap-server status codes. The bootstrap_*
 * functions return kern_return_t values; these are the well-known
 * success / "name not registered" / "name already in use" codes
 * Apple's libbootstrap exposes. Subset our consumers (libSC, the
 * launchd-port liblaunch) consume.
 */
#define BOOTSTRAP_SUCCESS                       0
#define BOOTSTRAP_NOT_PRIVILEGED                1100
#define BOOTSTRAP_NAME_IN_USE                   1101
#define BOOTSTRAP_UNKNOWN_SERVICE               1102
#define BOOTSTRAP_SERVICE_ACTIVE                1103
#define BOOTSTRAP_BAD_COUNT                     1104
#define BOOTSTRAP_NO_MEMORY                     1105

kern_return_t bootstrap_check_in(mach_port_t bp, const name_t name,
    mach_port_t *port);

kern_return_t bootstrap_look_up(mach_port_t bp, const name_t name,
    mach_port_t *port);

/*
 * Server entry point. Runs the bootstrap server message loop on the
 * given receive port until `*stop` is set to true. Service-name map
 * is per-call (state is internal to bootstrap_server_run for now —
 * future: persistent map shared across processes via shared memory
 * or a daemon).
 *
 * Phase G1: single-task only. Used by tests + the future daemon.
 */
void bootstrap_server_run(mach_port_t service_port, volatile int *stop);

#ifdef __cplusplus
}
#endif

#endif /* !_SERVERS_BOOTSTRAP_H_ */
