/* sys/fileport.h — FreeBSD shim. Apple's fileport API converts
 * between file descriptors and Mach send rights for atomic fd-passing
 * inside Mach message bodies (notifyd uses for delivery-side fd
 * registration). On XNU these are kernel Mach traps with kernel-side
 * fileglob tracking.
 *
 * For J1 compile-only: stub the declarations + return failure at
 * runtime. The real implementation is a J2 prereq in mach.ko via
 * new ops in our trap multiplexer (slot 219) — per
 * pkgdemon.github.io/freebsd-asl-plan.html §13.3, ~200-300 LoC
 * patterned after task_get/set_special_port from Phase G prereqs.
 */
#ifndef _FREEBSD_SHIM_SYS_FILEPORT_H_
#define _FREEBSD_SHIM_SYS_FILEPORT_H_

#include <mach/mach.h>
#include <mach/port.h>

/* fileport_t — Apple's typedef for a Mach port that wraps a file
 * descriptor. Same underlying type as mach_port_t (the value IS a
 * Mach port name); the typedef is purely for documentation. */
typedef mach_port_t fileport_t;

#define FILEPORT_NULL		((fileport_t)0)

/* Convert an fd to a Mach send right (fileglob-wrap kernel-side).
 * On FreeBSD (current stub): returns MACH_PORT_NULL via *port,
 * sets errno = ENOSYS. */
extern int fileport_makeport(int fd, mach_port_t *port);

/* Convert a Mach send right back to a new fd in the calling
 * process. Stub returns -1 with errno = ENOSYS. */
extern int fileport_makefd(mach_port_t port);

/* Invalidate a previously created fileport. Stub no-op. */
extern void fileport_invalidate(mach_port_t port);

#endif
