/*
 * mach_event_bridge.h — internal API for the pset/pipe wakeup bridge.
 * See mach_event_bridge.c for the architecture rationale.
 */
#ifndef _MACH_EVENT_BRIDGE_H_
#define _MACH_EVENT_BRIDGE_H_

#include <sys/types.h>
#include <sys/mach/port.h>

/* Forward declarations only — full struct definitions chain in headers
 * (ipc_port.h → rpc_common_data → ipc_pset.h) that we don't want to
 * impose on every includer of this file. mach_event_bridge.c and its
 * call sites include the chain in the right order. The signatures
 * below use `struct ipc_pset *` rather than the `ipc_pset_t` typedef
 * to avoid a duplicate-typedef collision with ipc_pset.h. */
struct thread;
struct ipc_pset;

/* Module init / destroy — called from mach_module.c. */
void mach_event_bridge_init(void);
void mach_event_bridge_destroy(void);

/* register_event_bell handler: register a pipe write-end with a pset. */
int  mach_event_bridge_register(struct thread *td,
    mach_port_name_t pset_name, int write_fd);

/* unregister_event_bell handler: unregister a bell by pset name. */
int  mach_event_bridge_unregister(struct thread *td,
    mach_port_name_t pset_name);

/* Called from pset destroy path. */
void mach_event_bridge_unregister_pset(struct ipc_pset *pset);

/* Called from ipc_pset_signal() when a message arrives. */
void mach_event_bridge_fire(struct ipc_pset *pset);

#endif /* !_MACH_EVENT_BRIDGE_H_ */
