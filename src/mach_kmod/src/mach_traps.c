/*-
 * Copyright (c) 2014-2015, Matthew Macy <mmacy@nextbsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *  2. Neither the name of Matthew Macy nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>

#include "mach_event_bridge.h"

__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/namei.h>
#include <sys/dirent.h>
#include <sys/eventhandler.h>
#include <sys/proc.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/filedesc.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/resourcevar.h>
#include <sys/socket.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <sys/unistd.h>
#include <sys/times.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/ptrace.h>
#include <sys/signalvar.h>

#include <netinet/in.h>
#include <sys/sysproto.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_extern.h>

#include <sys/mach/mach_types.h>

#include <sys/mach/ipc/ipc_types.h>
#include <sys/mach/ipc/ipc_kmsg.h>
#include <sys/mach/ipc/mach_msg.h>
#include <sys/mach/thread.h>
#include <sys/mach/mach_port_server.h>
#include <sys/mach/mach_vm_server.h>
#include <sys/mach/task_server.h>
#include <sys/mach/thread_switch.h>

#include <sys/mach/mach_init.h>

#include <sys/mach/mach_vm.h>
#include <sys/mach/mach.h>
#include <sys/mach/host.h>		/* realhost, host_lock */
#include <sys/mach/host_special_ports.h>	/* HOST_BOOTSTRAP_PORT */


#pragma clang diagnostic ignored "-Wunused-parameter"

int
sys_clock_sleep_trap(struct thread *td __unused, struct clock_sleep_trap_args *uap)
{

	td->td_retval[0] = clock_sleep(uap->clock_name, uap->sleep_type, uap->sleep_sec, uap->sleep_nsec, uap->wakeup_time);
	return (0);
}

int
sys_mach_timebase_info(struct thread *td __unused, struct mach_timebase_info_args *uap)
{

	return (mach_timebase_info(uap->info));
}

int
sys_mach_msg_overwrite_trap(struct thread *td __unused, struct mach_msg_overwrite_trap_args *uap)
{
	td->td_retval[0] = mach_msg_overwrite_trap(
		uap->msg, uap->option, uap->send_size, uap->rcv_size,
		uap->rcv_name, uap->timeout, uap->notify, uap->rcv_msg,
		uap->scatter_list_size);
	return (0);
}

int
sys_mach_msg_trap(struct thread *td __unused, struct mach_msg_trap_args *uap)
{
	struct mach_msg_overwrite_trap_args uap0;

	bcopy(uap, &uap0, sizeof(*uap));
	uap0.rcv_msg = NULL;
	return (sys_mach_msg_overwrite_trap(td, &uap0));
}
	
int
sys_semaphore_wait_trap(struct thread *td, struct semaphore_wait_trap_args *uap)
	UNSUPPORTED;

int
sys_semaphore_signal_trap(struct thread *td, struct semaphore_signal_trap_args *uap)
	UNSUPPORTED;
int
sys_semaphore_wait_signal_trap(struct thread *td, struct semaphore_wait_signal_trap_args *uap)
	UNSUPPORTED;

int
sys_semaphore_signal_thread_trap(struct thread *td, struct semaphore_signal_thread_trap_args *uap)
	UNSUPPORTED;

int
sys_semaphore_signal_all_trap(struct thread *td, struct semaphore_signal_all_trap_args *uap)
	UNSUPPORTED;

int
sys_task_for_pid(struct thread *td, struct task_for_pid_args *uap)
	UNSUPPORTED;

int
sys_thread_switch(struct thread *td __unused, struct thread_switch_args *uap)
{

	return (mach_thread_switch(uap->thread_name, uap->option, uap->option_time));
}	

static int
_swtch_pri(struct thread *td)
{
	thread_lock(td);
	if (td->td_state == TDS_RUNNING)
		td->td_proc->p_stats->p_cru.ru_nivcsw++;        /* XXXSMP */
	mi_switch(SW_VOL);
	thread_unlock(td);
	return (0);
}

int
sys_swtch_pri(struct thread *td, struct swtch_pri_args *uap __unused)
{
	
	return (_swtch_pri(td));
}

int
sys_swtch(struct thread *td, struct swtch_args *v __unused)
{

	return (_swtch_pri(td));
}

int
sys_mach_reply_port(struct thread *td, struct mach_reply_port_args *uap __unused)
{

	td->td_retval[0] = mach_reply_port();
	return (0);
}

int
sys_thread_self_trap(struct thread *td, struct thread_self_trap_args *uap)
{

	td->td_retval[0] = mach_thread_self();
	return (0);
}

int
sys_task_self_trap(struct thread *td, struct task_self_trap_args *uap)
{

	td->td_retval[0] = mach_task_self();
	return (0);
}

int
sys_host_self_trap(struct thread *td, struct host_self_trap_args *uap)
{

	td->td_retval[0] = mach_host_self();
	return (0);
}

/*
 * task_get_special_port / task_set_special_port — direct syscall
 * wrappers, not Apple-imported (Apple delivers these over MIG to the
 * task port). Used by libsystem_kernel's task_get_bootstrap_port /
 * task_set_bootstrap_port macros for the bootstrap server's
 * client-process port discovery (Phase G design).
 *
 * `target` is the task port name; we ignore it and use current_task()
 * — matches the existing mach_port_* trap pattern. `which` selects
 * which special-port slot per <sys/mach/task_special_ports.h>.
 */
int
sys_task_get_special_port_trap(struct thread *td,
    struct task_get_special_port_trap_args *uap)
{
	task_t task = current_task();
	ipc_port_t port;
	mach_port_name_t name;
	kern_return_t kr;

	kr = task_get_special_port(task, uap->which, &port);
	if (kr != KERN_SUCCESS) {
		td->td_retval[0] = kr;
		return (0);
	}

	/*
	 * task_get_special_port handed us a "naked" send right (made via
	 * ipc_port_copy_send / ipc_port_make_send in ipc_tt.c).
	 * ipc_port_copyout_send installs it in our IPC space and returns
	 * the user-visible name. On failure it releases the right itself
	 * and returns MACH_PORT_NAME_NULL / _DEAD.
	 */
	name = ipc_port_copyout_send(port, task->itk_space);
	if (copyout(&name, uap->port, sizeof(name)) != 0) {
		td->td_retval[0] = KERN_INVALID_ARGUMENT;
		return (0);
	}
	td->td_retval[0] = KERN_SUCCESS;
	return (0);
}

int
sys_task_set_special_port_trap(struct thread *td,
    struct task_set_special_port_trap_args *uap)
{
	task_t task = current_task();
	ipc_port_t port = IP_NULL;
	kern_return_t kr;

	/*
	 * uap->port is a userland mach_port_name_t (unsigned int), not the
	 * kernel-side mach_port_t (struct ipc_port *). Compare to 0
	 * directly — using MACH_PORT_NULL here would be a pointer/integer
	 * compare (kernel MACH_PORT_NULL is (mach_port_t)0). A 0 name is
	 * the "clear the slot" value.
	 */
	if (uap->port != 0) {
		kr = ipc_object_copyin(task->itk_space, uap->port,
		    MACH_MSG_TYPE_COPY_SEND, (ipc_object_t *)&port);
		if (kr != KERN_SUCCESS) {
			td->td_retval[0] = kr;
			return (0);
		}
	}

	/*
	 * task_set_special_port stores `port` directly in the task's
	 * itk_<slot> field and releases whatever was there before. It
	 * takes ownership of the send right we just copied in.
	 */
	td->td_retval[0] = task_set_special_port(task, uap->which, port);
	return (0);
}

/*
 * host_set_special_port — set a slot in the host's special-port array.
 * Dedicated syscall (each Mach trap gets its own FreeBSD slot from the
 * widened lkmnosys band). `host` is the host port name; we ignore it
 * and operate on realhost — matches the existing trap pattern.
 */
int
sys_host_set_special_port_trap(struct thread *td,
    struct host_set_special_port_trap_args *uap)
{
	task_t task = current_task();
	ipc_port_t port = IP_NULL;
	ipc_port_t old;
	kern_return_t kr;

	/* Only HOST_BOOTSTRAP_PORT is settable from userland at the
	 * moment — kernel-provided slots (HOST_PORT etc.) and other
	 * Apple-defined slots are rejected. */
	if (uap->which != HOST_BOOTSTRAP_PORT) {
		td->td_retval[0] = KERN_INVALID_ARGUMENT;
		return (0);
	}

	if (uap->port != 0) {
		kr = ipc_object_copyin(task->itk_space, uap->port,
		    MACH_MSG_TYPE_COPY_SEND, (ipc_object_t *)&port);
		if (kr != KERN_SUCCESS) {
			td->td_retval[0] = kr;
			return (0);
		}
	}

	host_lock(&realhost);
	old = realhost.special[uap->which];
	realhost.special[uap->which] = port;
	host_unlock(&realhost);

	if (IP_VALID(old))
		ipc_port_release_send(old);

	td->td_retval[0] = KERN_SUCCESS;
	return (0);
}

/*
 * register_event_bell / unregister_event_bell — Task #39 Path B.
 * Dedicated syscalls wrapping the pset/pipe wakeup bridge. See
 * src/mach_kmod/src/mach_event_bridge.c.
 */
int
sys_register_event_bell_trap(struct thread *td,
    struct register_event_bell_trap_args *uap)
{

	td->td_retval[0] = mach_event_bridge_register(td, uap->port,
	    uap->write_fd);
	return (0);
}

int
sys_unregister_event_bell_trap(struct thread *td,
    struct unregister_event_bell_trap_args *uap)
{

	td->td_retval[0] = mach_event_bridge_unregister(td, uap->port);
	return (0);
}

int
sys__kernelrpc_mach_port_allocate_trap(struct thread *td __unused, struct _kernelrpc_mach_port_allocate_trap_args *uap)
{
	ipc_space_t space = current_task()->itk_space;
	mach_port_name_t name;
	int error;

	if ((error = mach_port_allocate(space, uap->right, &name)) != 0)
		return (error);
	
	return (copyout(&name, uap->name, sizeof(*uap->name)));
}

int
sys__kernelrpc_mach_port_deallocate_trap(struct thread *td, struct _kernelrpc_mach_port_deallocate_trap_args *uap)
{
	ipc_space_t space = current_task()->itk_space;

	mach_port_deallocate(space, uap->name);
	return (0);
}

int
sys__kernelrpc_mach_port_insert_right_trap(struct thread *td, struct _kernelrpc_mach_port_insert_right_trap_args *uap)
{
	task_t task = current_task(); /* port_name_to_task(uap->target); */
	ipc_port_t port;
	mach_msg_type_name_t disp;
	int rv = MACH_SEND_INVALID_DEST;

	if (task != current_task())
		goto done;
	if (uap->name != uap->poly)
		goto done;
	rv = ipc_object_copyin(task->itk_space, uap->poly, uap->polyPoly, (ipc_object_t *)&port);
	if (rv != KERN_SUCCESS)
		goto done;
	disp = ipc_object_copyin_type(uap->polyPoly);
	rv = mach_port_insert_right(task->itk_space, uap->name, port, disp);
done:
	if (task)
		task_deallocate(task);
	td->td_retval[0] = rv;
	return (0);
}

int
sys__kernelrpc_mach_port_mod_refs_trap(struct thread *td, struct _kernelrpc_mach_port_mod_refs_trap_args *uap)
{
	ipc_space_t space = current_task()->itk_space;
	/*
	  mach_port_name_t target = uap->target;
	  mach_port_name_t *name = uap->name;
	  mach_port_right_t right = uap->right;
	  mach_port_delta_t delta = uap->delta;
	*/
	td->td_retval[0] = mach_port_mod_refs(space, uap->name, uap->right, uap->delta);
	return (0);
}

int
sys__kernelrpc_mach_port_move_member_trap(struct thread *td, struct _kernelrpc_mach_port_move_member_trap_args *uap)
{
	ipc_space_t space = current_task()->itk_space;

	td->td_retval[0] = mach_port_move_member(space, uap->member, uap->after);
	return (0);
}

int
sys__kernelrpc_mach_port_insert_member_trap(struct thread *td, struct _kernelrpc_mach_port_insert_member_trap_args *uap)
{
	ipc_space_t space = current_task()->itk_space;

	td->td_retval[0] = mach_port_move_member(space, uap->name, uap->pset);
	return (0);
}

int
sys__kernelrpc_mach_port_extract_member_trap(struct thread *td, struct _kernelrpc_mach_port_extract_member_trap_args *uap)
{
	ipc_space_t space = current_task()->itk_space;

	td->td_retval[0] = mach_port_move_member(space, uap->name, MACH_PORT_NAME_NULL);
	return (0);
}

int
sys__kernelrpc_mach_port_construct_trap(struct thread *td, struct _kernelrpc_mach_port_construct_trap_args *uap)
	UNSUPPORTED;

int
sys__kernelrpc_mach_port_destruct_trap(struct thread *td, struct _kernelrpc_mach_port_destruct_trap_args *uap)
	UNSUPPORTED;

int
sys__kernelrpc_mach_port_destroy_trap(struct thread *td, struct _kernelrpc_mach_port_destroy_trap_args *uap)
{
	ipc_space_t space = current_task()->itk_space;

	return (mach_port_destroy(space, uap->name));
}

int
sys__kernelrpc_mach_port_guard_trap(struct thread *td, struct _kernelrpc_mach_port_guard_trap_args *uap)
	UNSUPPORTED;


int
sys__kernelrpc_mach_port_unguard_trap(struct thread *td, struct _kernelrpc_mach_port_unguard_trap_args *uap)
	UNSUPPORTED;

int
sys__kernelrpc_mach_vm_map_trap(struct thread *td, struct _kernelrpc_mach_vm_map_trap_args *uap)
{
	int error;
	vm_offset_t addr;

	if ((error = copyin(uap->address, &addr, sizeof(addr))) != 0)
		return (error);
	error = mach_vm_map(&curthread->td_proc->p_vmspace->vm_map, &addr, uap->size, uap->mask, uap->flags, NULL, 0, 0, uap->cur_protection,
						VM_PROT_ALL, VM_INHERIT_NONE);
	if (error)
		return (error);
	return (copyout(&addr, uap->address, sizeof(addr)));
}

int
sys__kernelrpc_mach_vm_allocate_trap(struct thread *td, struct _kernelrpc_mach_vm_allocate_trap_args *uap)
{
	/* mach_port_name_t target = uap->target; current task only */
	mach_vm_offset_t *address = uap->address;
	mach_vm_offset_t uaddr;
	mach_vm_size_t size = uap->size;
	int flags = uap->flags;
	int error;

	if ((error = copyin(address, &uaddr, sizeof(mach_vm_offset_t))))
		return (error);

	if ((error = mach_vm_allocate(&td->td_proc->p_vmspace->vm_map,
								  &uaddr, size, flags)))
		return (error);
	if ((error = copyout(&uaddr, address, sizeof(mach_vm_offset_t))))
		return (error);
	return (0);
}

int
sys__kernelrpc_mach_vm_deallocate_trap(struct thread *td, struct _kernelrpc_mach_vm_deallocate_trap_args *uap)
{
	/* mach_port_name_t target = uap->target; current task only */

	return (mach_vm_deallocate(&td->td_proc->p_vmspace->vm_map, uap->address, uap->size));
}

int
sys__kernelrpc_mach_vm_protect_trap(struct thread *td, struct _kernelrpc_mach_vm_protect_trap_args *uap)
{
	/* mach_port_name_t target = uap->target */
	/* int set_maximum = uap->set_maximum */

	return (mach_vm_protect(&td->td_proc->p_vmspace->vm_map, uap->address, uap->size, FALSE, uap->new_protection));
}

