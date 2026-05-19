/*
 * mach_event_bridge.h — internal API for the pset/pipe wakeup bridge.
 * See mach_event_bridge.c for the architecture rationale.
 */
#ifndef _MACH_EVENT_BRIDGE_H_
#define _MACH_EVENT_BRIDGE_H_

#include <sys/types.h>
#include <sys/mach/port.h>
#include <sys/mach/ipc/ipc_pset.h>

struct thread;

/* Module init / destroy — called from mach_module.c. */
void mach_event_bridge_init(void);
void mach_event_bridge_destroy(void);

/* Trap-mux op 4 handler: register a pipe write-end with a pset. */
int  mach_event_bridge_register(struct thread *td,
    mach_port_name_t pset_name, int write_fd);

/* Called from pset destroy path. */
void mach_event_bridge_unregister_pset(ipc_pset_t pset);

/* Called from ipc_pset_signal() when a message arrives. */
void mach_event_bridge_fire(ipc_pset_t pset);

#endif /* !_MACH_EVENT_BRIDGE_H_ */
