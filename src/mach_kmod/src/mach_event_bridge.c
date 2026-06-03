/*
 * mach_event_bridge.c — pset-to-pipe wakeup bridge for libdispatch.
 *
 * Architecture (task #39 / Path B, design notes 2026-05-19):
 *
 *   Userland (libdispatch) wants to wait on Mach-port message arrivals
 *   integrated with its main kqueue thread, but FreeBSD's kqueue has
 *   no EVFILT_MACHPORT slot available (EVFILT_SYSCOUNT is 15, all slots
 *   -1..-15 are taken by base filters, and we're not modifying the
 *   FreeBSD base kernel).
 *
 *   Bridge solution:
 *     1. libdispatch creates a pipe() pair.
 *     2. libdispatch creates a Mach port set, adds its receive ports
 *        to it.
 *     3. libdispatch calls the register_event_bell syscall (pset,
 *        write_fd) to register the pipe's write-end with mach.ko,
 *        associated with the pset.
 *     4. libdispatch registers EVFILT_READ on the pipe's read-end
 *        in its main kqueue.
 *     5. When a Mach message arrives on any port in the pset,
 *        ipc_pset_signal() (in ipc_pset.c) calls
 *        mach_event_bridge_fire(pset), which writes one byte to the
 *        registered pipe's write-end via the stored struct file *.
 *     6. The byte makes the pipe's read-end readable; libdispatch's
 *        main kqueue thread wakes via EVFILT_READ, drains the pipe,
 *        and drains the pset via mach_msg(MACH_RCV_TIMEOUT 0, pset).
 *
 * MIG/syscall preference (memory:freebsd-mach-architecture-prefs):
 *   The registration operation is a dedicated mach.ko syscall
 *   (register_event_bell) rather than a /dev/mach ioctl. A future
 *   migration to MIG-over-mach_msg on a kernel-side mach port is
 *   tracked separately.
 */

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/proc.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/uio.h>
#include <sys/capsicum.h>

#include <sys/mach/port.h>
#include <sys/mach/kern_return.h>
#include <sys/mach/mach_types.h>
#include <sys/mach/ipc/ipc_object.h>	/* rpc_common_data via ipc_object */
#include <sys/mach/ipc/ipc_port.h>	/* required before ipc_pset.h */
#include <sys/mach/ipc/ipc_pset.h>
#include <sys/mach/ipc/ipc_entry.h>
#include <sys/mach/ipc/ipc_space.h>
#include <sys/mach/thread.h>		/* current_task() */

#include "mach_event_bridge.h"

MALLOC_DEFINE(M_MACH_EVENT_BELL, "mach_event_bell", "mach_event_bridge bells");

struct mach_event_bell {
	LIST_ENTRY(mach_event_bell)	 meb_link;
	ipc_pset_t			 meb_pset;	/* the port set this bell watches (not a held ref — pset destroy path calls unregister) */
	struct file			*meb_fp;	/* write-end of userland's pipe; held via fhold() */
};

static LIST_HEAD(, mach_event_bell)	mach_event_bells =
    LIST_HEAD_INITIALIZER(mach_event_bells);
static struct mtx			mach_event_bells_lock;

extern int mach_debug_enable;

void
mach_event_bridge_init(void)
{
	mtx_init(&mach_event_bells_lock, "mach_event_bells", NULL, MTX_DEF);
}

void
mach_event_bridge_destroy(void)
{
	struct mach_event_bell *bell;

	mtx_lock(&mach_event_bells_lock);
	while ((bell = LIST_FIRST(&mach_event_bells)) != NULL) {
		LIST_REMOVE(bell, meb_link);
		if (bell->meb_fp != NULL)
			fdrop(bell->meb_fp, curthread);
		free(bell, M_MACH_EVENT_BELL);
	}
	mtx_unlock(&mach_event_bells_lock);
	mtx_destroy(&mach_event_bells_lock);
}

/*
 * Look up an existing bell for the given pset. Caller must hold the
 * bells lock.
 */
static struct mach_event_bell *
mach_event_bell_find_locked(ipc_pset_t pset)
{
	struct mach_event_bell *bell;

	mtx_assert(&mach_event_bells_lock, MA_OWNED);
	LIST_FOREACH(bell, &mach_event_bells, meb_link) {
		if (bell->meb_pset == pset)
			return (bell);
	}
	return (NULL);
}

/*
 * Register a bell: associate the named pset with the file descriptor
 * for the write-end of userland's wakeup pipe. Replaces any existing
 * bell for the same pset.
 *
 * Returns 0 on success, errno on failure (and sets *retval to
 * KERN_INVALID_ARGUMENT etc. for downstream Mach-style return).
 */
int
mach_event_bridge_register(struct thread *td, mach_port_name_t pset_name,
    int write_fd)
{
	ipc_space_t		space = current_task()->itk_space;
	ipc_pset_t		pset = IPS_NULL;
	struct mach_event_bell	*bell, *old;
	struct file		*fp = NULL;
	kern_return_t		kr;
	int			error;
	cap_rights_t		rights;

	if (write_fd < 0)
		return (EBADF);

	kr = ipc_object_translate(space, pset_name, MACH_PORT_RIGHT_PORT_SET,
	    (ipc_object_t *)&pset);
	if (kr != KERN_SUCCESS)
		return (kr == KERN_INVALID_NAME ? ENOENT : EINVAL);
	/* ipc_object_translate locked the pset; release the lock — we just
	 * need the pointer as an identity key. */
	ips_unlock(pset);

	/* Hold a reference to the userland pipe's write-end struct file *.
	 * fget_write checks cap rights for write capability. */
	error = fget_write(td, write_fd, cap_rights_init(&rights, CAP_WRITE),
	    &fp);
	if (error != 0)
		return (error);

	bell = malloc(sizeof(*bell), M_MACH_EVENT_BELL, M_WAITOK | M_ZERO);
	bell->meb_pset = pset;
	bell->meb_fp = fp;

	mtx_lock(&mach_event_bells_lock);
	old = mach_event_bell_find_locked(pset);
	if (old != NULL) {
		LIST_REMOVE(old, meb_link);
	}
	LIST_INSERT_HEAD(&mach_event_bells, bell, meb_link);
	mtx_unlock(&mach_event_bells_lock);

	if (old != NULL) {
		if (old->meb_fp != NULL)
			fdrop(old->meb_fp, td);
		free(old, M_MACH_EVENT_BELL);
	}

	if (mach_debug_enable) {
		printf("[T39-bell] register pset=%p fd=%d fp=%p\n", pset,
		    write_fd, fp);
	}
	return (0);
}

/*
 * Unregister a bell by pset name. unregister_event_bell handler. Userland
 * (the libmach EVFILT_MACHPORT wrapper) calls this from reg_destroy
 * when a dispatch source is torn down, BEFORE closing the wakeup
 * pipe. Without it the kernel bell outlives the pipe — the next
 * message fires the bell, fo_write hits a closed pipe (EPIPE), and
 * the wakeup is silently lost. The bell is otherwise only cleaned
 * up on pset destroy, which a re-registering libdispatch source
 * (EV_DELETE then EV_ADD) doesn't trigger.
 *
 * Returns 0 on success, errno on failure.
 */
int
mach_event_bridge_unregister(struct thread *td, mach_port_name_t pset_name)
{
	ipc_space_t	space = current_task()->itk_space;
	ipc_pset_t	pset = IPS_NULL;
	kern_return_t	kr;

	(void)td;
	kr = ipc_object_translate(space, pset_name, MACH_PORT_RIGHT_PORT_SET,
	    (ipc_object_t *)&pset);
	if (kr != KERN_SUCCESS)
		return (kr == KERN_INVALID_NAME ? ENOENT : EINVAL);
	ips_unlock(pset);

	mach_event_bridge_unregister_pset(pset);
	return (0);
}

/*
 * Unregister a bell for the given pset. Called from the pset destroy
 * path; safe to call when no bell exists.
 */
void
mach_event_bridge_unregister_pset(ipc_pset_t pset)
{
	struct mach_event_bell *bell;

	mtx_lock(&mach_event_bells_lock);
	bell = mach_event_bell_find_locked(pset);
	if (bell != NULL)
		LIST_REMOVE(bell, meb_link);
	mtx_unlock(&mach_event_bells_lock);

	if (bell != NULL) {
		if (mach_debug_enable) {
			printf("[T39-bell] unregister pset=%p fp=%p\n", pset,
			    bell->meb_fp);
		}
		if (bell->meb_fp != NULL)
			fdrop(bell->meb_fp, curthread);
		free(bell, M_MACH_EVENT_BELL);
	}
}

/*
 * Fire the bell for the given pset: write one byte to the registered
 * pipe's write-end. Called from ipc_pset_signal() in the message-
 * arrival path. Best-effort — failures are logged but not propagated
 * (the receiver may have closed the read-end already, the pipe may
 * be full, etc.).
 */
void
mach_event_bridge_fire(ipc_pset_t pset)
{
	struct mach_event_bell *bell;
	struct file		*fp;
	struct iovec		 iov;
	struct uio		 auio;
	char			 byte = 0x01;
	int			 error;

	mtx_lock(&mach_event_bells_lock);
	bell = mach_event_bell_find_locked(pset);
	if (bell == NULL) {
		mtx_unlock(&mach_event_bells_lock);
		return;
	}
	/* Hold an extra ref so we can release the bells lock before the
	 * potentially-sleeping fo_write call. fhold() is __warn_unused_result
	 * on FreeBSD 15 (returns true on success); we don't have a useful
	 * fallback path if it fails, so cast away. */
	fp = bell->meb_fp;
	(void)fhold(fp);
	mtx_unlock(&mach_event_bells_lock);

	iov.iov_base = &byte;
	iov.iov_len = 1;
	auio.uio_iov = &iov;
	auio.uio_iovcnt = 1;
	auio.uio_offset = -1;
	auio.uio_resid = 1;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_rw = UIO_WRITE;
	auio.uio_td = curthread;

	error = fo_write(fp, &auio, curthread->td_ucred, 0, curthread);
	if (error != 0 && error != EAGAIN && mach_debug_enable) {
		printf("[T39-bell] fire pset=%p fo_write=%d\n", pset, error);
	}
	fdrop(fp, curthread);
}
