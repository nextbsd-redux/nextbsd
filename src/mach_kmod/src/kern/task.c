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
/*
 * Copyright 1991-1998 by Open Software Foundation, Inc. 
 *              All Rights Reserved 
 *  
 * Permission to use, copy, modify, and distribute this software and 
 * its documentation for any purpose and without fee is hereby granted, 
 * provided that the above copyright notice appears in all copies and 
 * that both the copyright notice and this permission notice appear in 
 * supporting documentation. 
 *  
 * OSF DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE 
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
 * FOR A PARTICULAR PURPOSE. 
 *  
 * IN NO EVENT SHALL OSF BE LIABLE FOR ANY SPECIAL, INDIRECT, OR 
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM 
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN ACTION OF CONTRACT, 
 * NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION 
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. 
 */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989,1988 Carnegie Mellon University
 * All Rights Reserved.
 * 
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 * 
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 * 
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/*
 *	File:	kern/task.c
 *	Author:	Avadis Tevanian, Jr., Michael Wayne Young, David Golub,
 *		David Black
 *
 *	Task management primitives implementation.
 */
/*
 * Copyright (c) 1993 The University of Utah and
 * the Computer Systems Laboratory (CSL).  All rights reserved.
 *
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 *
 * THE UNIVERSITY OF UTAH AND CSL ALLOW FREE USE OF THIS SOFTWARE IN ITS "AS
 * IS" CONDITION.  THE UNIVERSITY OF UTAH AND CSL DISCLAIM ANY LIABILITY OF
 * ANY KIND FOR ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 *
 * CSL requests users of this software to return to csl-dist@cs.utah.edu any
 * improvements that they make and grant CSL redistribution rights.
 *
 */

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/eventhandler.h>
#include <sys/kernel.h>
#include <sys/mach/vm_types.h>

#include <sys/mach/task_info.h>
#include <sys/mach/task_special_ports.h>
#include <sys/mach/mach_types.h>
#include <sys/mach/rpc.h>
#include <sys/mach/ipc/ipc_space.h>
#include <sys/mach/ipc/ipc_entry.h>
#include <sys/mach/mach_param.h>

#include <sys/mach/task.h>
#include <sys/mach/ipc/ipc_kmsg.h>
#include <sys/mach/thread.h>

#include <sys/mach/sched_prim.h>	/* for thread_wakeup */
#include <sys/mach/ipc_tt.h>
#if 0
#include <sys/mach/ledger.h>
#endif
#include <sys/mach/host_special_ports.h>
#include <sys/mach/host.h>
#include <vm/vm_kern.h>		/* for kernel_map, ipc_kernel_map */
#include <vm/uma.h>
#if	MACH_KDB
#include <ddb/db_sym.h>
#endif	/* MACH_KDB */

#include <sys/mach/queue.h>
/*
 * Exported interfaces
 */
#include <sys/mach/task_server.h>
#include <sys/mach/mach_host_server.h>
#include <sys/mach/mach_port_server.h>

struct processor_set default_pset;
unsigned	int sched_ticks;
task_t	kernel_task;
uma_zone_t	task_zone;

/* Forwards */


kern_return_t	task_hold_locked(
			task_t		task);
void		task_wait_locked(
			task_t		task);
kern_return_t	task_release(
			task_t		task);
void		task_act_iterate(
			task_t		 task,
			kern_return_t	(*func)(thread_act_t inc));
void		task_free(
			task_t		task );
void		task_synchronizer_destroy_all(
			task_t		task);


kern_return_t
task_create(
	task_t				parent_task,
	__unused ledger_array_t	ledger_ports,
	__unused mach_msg_type_number_t	num_ledger_ports,
	__unused boolean_t		inherit_memory,
	__unused task_t			*child_task)	/* OUT */
{
	if (parent_task == TASK_NULL)
		return(KERN_INVALID_ARGUMENT);

	return(KERN_FAILURE);
}


static kern_return_t
task_create_internal(
	task_t		new_task)
{

	/* one ref for just being alive; one for our caller */
	new_task->ref_count = 2;
	new_task->semaphores_owned = 0;

	ipc_task_create(new_task);

	return (KERN_SUCCESS);
}

static kern_return_t
task_init_internal(
	task_t		parent_task,
	task_t		new_task)
{
#ifdef notyet
	register processor_set_t	pset;
#endif

	/* one ref for just being alive; one for our caller */
	new_task->ref_count = 2;
	new_task->semaphores_owned = 0;

	ipc_task_init(new_task, parent_task);

	if (parent_task != TASK_NULL) {
#ifdef notyet		
		pset = parent_task->processor_set;
		if (!pset->active)
			pset = &default_pset;
#endif
		set_security_token(new_task);
		new_task->policy = parent_task->policy;
	} else {
#ifdef notyet
		pset = &default_pset;
#endif
		new_task->policy = POLICY_TIMESHARE;
		new_task->sec_token = KERNEL_SECURITY_TOKEN;
		new_task->audit_token = KERNEL_AUDIT_TOKEN;
	}
	ipc_task_enable(new_task);
	return(KERN_SUCCESS);
}



/*
 *	task_free:
 *
 *	Called by task_deallocate when the task's reference count drops to zero.
 *	Task is locked.
 */
void
task_free( register task_t	task )
{
	/* tasks are tied to proc structures so should only be freed if proc goes away */
	task_unlock(task);
	return;
	/*
	 * Temporarily restore the reference we dropped above, then
	 * freeze the task so that the task->processor_set field
	 * cannot change. In the !MACH_HOST case, the logic can be
	 * simplified, since the default_pset is the only pset.
	 */
	++task->ref_count;
	task_unlock(task);
	task_lock(task);
	if (--task->ref_count > 0) {
		/*
		 * A new reference appeared (probably from the pset).
		 * Back out. Must unfreeze inline since we'already
		 * dropped our reference.
		 */
		task_unlock(task);
		return;
	}
	task_unlock(task);
	is_release(task->itk_space);
	uma_zfree(task_zone, task);
}


void
task_deallocate( task_t task )
{
	if (task != TASK_NULL) {
	    int x;
	    task_lock(task);
	    x = --task->ref_count;
	    if (x == 0)
		task_free(task);	/* unlocks task */
	    else
		task_unlock(task);
	}
}

void
task_reference( register task_t task )
{
	if (task != TASK_NULL) {
	    task_lock(task);
	    task->ref_count++;
	    task_unlock(task);
	}
}

/*
 *	task_terminate:
 *
 *	Terminate the specified task.  See comments on thread_terminate
 *	(kern/thread.c) about problems with terminating the "current task."
 */
kern_return_t
task_terminate(
	register task_t	task)
{
#ifdef notyet	
	register thread_t	thread, cur_thread;
#if 0
	register queue_head_t	*list;
#endif	
	register task_t		cur_task;
	thread_act_t		thr_act, cur_thr_act;

	if (task == TASK_NULL)
		return(KERN_INVALID_ARGUMENT);

	assert(task != kernel_task);
#if 0
	list = &task->thr_acts;
	cur_task = current_task();
	cur_thr_act = current_thread()->top_act;
#endif

	/*
	 *	Deactivate task so that it can't be terminated again,
	 *	and so lengthy operations in progress will abort.
	 *
	 *	If the current thread is in this task, remove it from
	 *	the task's thread list to keep the thread-termination
	 *	loop simple.
	 */
	if (task == cur_task) {
		task_lock(task);
		if (!task->active) {
			/*
			 *	Task is already being terminated.
			 */
			task_unlock(task);
			thread_block();
			return(KERN_FAILURE);
		}

		task_hold_locked(task);
#if 0
		task->active = FALSE;

		/*
		 *	Make sure current thread is not being terminated.
		 */
		mutex_lock(&task->act_list_lock);
		cur_thread = act_lock_thread(cur_thr_act);
		if (!cur_thr_act->active) {
			act_unlock_thread(cur_thr_act);
			mutex_unlock(&task->act_list_lock);
			task_unlock(task);
			thread_terminate(cur_thr_act);
			return(KERN_FAILURE);
		}

		/*
		 * make sure that this thread is the last one in the list
		 */
		queue_remove(list, cur_thr_act, thread_act_t, thr_acts);
		queue_enter(list, cur_thr_act, thread_act_t, thr_acts);
		act_unlock_thread(cur_thr_act);
		mutex_unlock(&task->act_list_lock);
		/*
		 *	Shut down this thread's ipc now because it must
		 *	be left alone to terminate the task.
		 */
		ipc_thr_act_disable(cur_thr_act);
		ipc_thr_act_terminate(cur_thr_act);
#endif
	}
	else {
		/*
		 *	Lock both current and victim task to check for
		 *	potential deadlock.
		 */
		if (task < cur_task) {
			task_lock(task);
			task_lock(cur_task);
		}
		else {
			task_lock(cur_task);
			task_lock(task);
		}
		/*
		 *	Check if current thread_act or task is being terminated.
		 */
		cur_thread = act_lock_thread(cur_thr_act);
		if ((!cur_task->active) || (!cur_thr_act->active)) {
			/*
			 * Current task or thread is being terminated.
			 */
			act_unlock_thread(cur_thr_act);
			task_unlock(task);
			task_unlock(cur_task);
			return(KERN_FAILURE);
		}
		act_unlock_thread(cur_thr_act);
		task_unlock(cur_task);

		if (!task->active) {
			/*
			 *	Task is already being terminated.
			 */
			task_unlock(task);
			thread_block();
			return(KERN_FAILURE);
		}
		task_hold_locked(task);
		task->active = FALSE;
	}

	/*
	 *	Prevent further execution of the task.  ipc_task_disable
	 *	prevents further task operations via the task port.
	 *	If this is the current task, the current thread will
	 *	be left running.
	 */
	ipc_task_disable(task);
	task_wait_locked(task);

	/*
	 *	Terminate each thread in the task.  Depending on the
	 *	state of the thread, this can mean a number of things.
	 *	However, we just call thread_terminate(), which
	 *	takes care of all cases (see that code for details).
	 *
         *      The task_port is closed down, so no more thread_create
         *      operations can be done.  Thread_terminate closes the
         *      thread port for each thread; when that is done, the
         *      thread will eventually disappear.  Thus the loop will
         *      terminate.
	 *	Need to call thread_block() inside loop because some
         *      other thread (e.g., the reaper) may have to run to get rid
         *      of all references to the thread; it won't vanish from
         *      the task's thread list until the last one is gone.
         */
        while (!queue_empty(list)) {
                thr_act = (thread_act_t) queue_first(list);
                act_reference(thr_act);
                task_unlock(task);
                thread_terminate(thr_act);
                act_deallocate(thr_act);
                task_lock(task);
        }
        task_unlock(task);
#endif
	/*
	 *	Destroy all synchronizers owned by the task.
	 */
	task_synchronizer_destroy_all(task);

	/*
	 *	Shut down IPC.
	 */
	ipc_task_terminate(task);

	/*
	 *	Deallocate the task's reference to itself.
	 */
	task_deallocate(task);

	return(KERN_SUCCESS);
}

/*
 *	task_hold_locked:
 *
 *	Suspend execution of the specified task.
 *	This is a recursive-style suspension of the task, a count of
 *	suspends is maintained.
 *
 * 	CONDITIONS: the task is locked.
 */
kern_return_t
task_hold_locked(
	register task_t	task)
{
#if 0
	register queue_head_t	*list;
	register thread_act_t	thr_act, cur_thr_act;

	cur_thr_act = current_act();
#endif	

	if (!task->active) {
		return(KERN_FAILURE);
	}
#if 0

	task->suspend_count++;
	/*
	 *	Iterate through all the thread_act's and hold them.
	 *	Do not hold the current thread_act if it is within the
	 *	task.
	 */
	list = &task->thr_acts;
	thr_act = (thread_act_t) queue_first(list);
	while (!queue_end(list, (queue_entry_t) thr_act)) {
		(void)act_lock_thread(thr_act);
		thread_hold(thr_act);
		act_unlock_thread(thr_act);
		thr_act = (thread_act_t) queue_next(&thr_act->thr_acts);
	}
#endif	
	return(KERN_SUCCESS);
}


kern_return_t
task_release(
	register task_t	task)
{
#if 0
	register queue_head_t	*list;
	register thread_act_t	thr_act, next;

	task_lock(task);
	if (!task->active) {
		task_unlock(task);
		return(KERN_FAILURE);
	}

	task->suspend_count--;

	/*
	 *	Iterate through all the thread_act's and release them.
	 */
	list = &task->thr_acts;
	thr_act = (thread_act_t) queue_first(list);
	while (!queue_end(list, (queue_entry_t) thr_act)) {
		next = (thread_act_t) queue_next(&thr_act->thr_acts);
		(void)act_lock_thread(thr_act);
		thread_release(thr_act);
		act_unlock_thread(thr_act);
		thr_act = next;
	}
	task_unlock(task);
#endif	
	return(KERN_SUCCESS);
}

kern_return_t
task_threads(
	task_t			task,
	thread_act_array_t	*thr_act_list,
	mach_msg_type_number_t	*count)
{
#if 0
	unsigned int		actual;	/* this many thr_acts */
	thread_act_t		thr_act;
	thread_act_t		*thr_acts;
	thread_t		thread;
	int			i, j;
	boolean_t rt = FALSE; /* ### This boolean is FALSE, because there
			       * currently exists no mechanism to determine
			       * whether or not the reply port is an RT port
			       */


	vm_size_t size, size_needed;
	vm_offset_t addr;

	if (task == TASK_NULL)
		return KERN_INVALID_ARGUMENT;

	size = 0; addr = 0;

	for (;;) {
		task_lock(task);
		if (!task->active) {
			task_unlock(task);
			if (size != 0)
				KFREE(addr, size, rt);
			return KERN_FAILURE;
		}

		actual = task->thr_act_count;

		/* do we have the memory we need? */
		size_needed = actual * sizeof(mach_port_t);
		if (size_needed <= size)
			break;

		/* unlock the task and allocate more memory */
		task_unlock(task);

		if (size != 0)
			KFREE(addr, size, rt);

		assert(size_needed > 0);
		size = size_needed;

		addr = KALLOC(size, rt);
		if (addr == 0)
			return KERN_RESOURCE_SHORTAGE;
	}

	/* OK, have memory and the task is locked & active */
	thr_acts = (thread_act_t *) addr;

	for (i = j = 0, thr_act = (thread_act_t) queue_first(&task->thr_acts);
	     i < actual;
	     i++, thr_act = (thread_act_t) queue_next(&thr_act->thr_acts)) {
		act_reference(thr_act);
		thr_acts[j++] = thr_act;
	}
	assert(queue_end(&task->thr_acts, (queue_entry_t) thr_act));
	actual = j;

	/* can unlock task now that we've got the thr_act refs */
	task_unlock(task);

	if (actual == 0) {
		/* no thr_acts, so return null pointer and deallocate memory */

		*thr_act_list = 0;
		*count = 0;

		if (size != 0)
			KFREE(addr, size, rt);
	} else {
		/* if we allocated too much, must copy */

		if (size_needed < size) {
			vm_offset_t newaddr;

			newaddr = KALLOC(size_needed, rt);
			if (newaddr == 0) {
				for (i = 0; i < actual; i++)
					act_deallocate(thr_acts[i]);
				KFREE(addr, size, rt);
				return KERN_RESOURCE_SHORTAGE;
			}

			bcopy((char *) addr, (char *) newaddr, size_needed);
			KFREE(addr, size, rt);
			thr_acts = (thread_act_t *) newaddr;
		}

		*thr_act_list = (mach_port_t *) thr_acts;
		*count = actual;

		/* do the conversion that Mig should handle */

		for (i = 0; i < actual; i++)
			((ipc_port_t *) thr_acts)[i] =
				convert_act_to_port(thr_acts[i]);
	}
#endif
	return KERN_SUCCESS;
}

kern_return_t
task_suspend(
	register task_t		task)
{
#if 0
	if (task == TASK_NULL)
		return (KERN_INVALID_ARGUMENT);

	task_lock(task);
	if (!task->active) {
		task_unlock(task);
		return (KERN_FAILURE);
	}
	if ((task->user_stop_count)++ > 0) {
		/*
		 *	If the stop count was positive, the task is
		 *	already stopped and we can exit.
		 */
		task_unlock(task);
		return (KERN_SUCCESS);
	}

	/*
	 *	Hold all of the threads in the task, and wait for
	 *	them to stop.  If the current thread is within
	 *	this task, hold it separately so that all of the
	 *	other threads can stop first.
	 */
	if (task_hold_locked(task) != KERN_SUCCESS) {
		task_unlock(task);
		return (KERN_FAILURE);
	}

	task_wait_locked(task);
	task_unlock(task);
#endif
	return (KERN_SUCCESS);
}

/*
 * Wait for all threads in task to stop.  Called with task locked.
 */
void
task_wait_locked(
	register task_t		task)
{
	#if 0
	register queue_head_t	*list;
	register thread_act_t	thr_act, refd_thr_act;
	register thread_t	thread, cur_thr;

	cur_thr = current_thread();
	/*
	 *	Iterate through all the thread's and wait for them to
	 *	stop.  Do not wait for the current thread if it is within
	 *	the task.
	 */
	list = &task->thr_acts;
	refd_thr_act = THR_ACT_NULL;
	while (1) {
		thr_act = (thread_act_t) queue_first(list);
		while (!queue_end(list, (queue_entry_t) thr_act)) {
			thread = act_lock_thread(thr_act);
			if (refd_thr_act != THR_ACT_NULL) {
				act_deallocate(refd_thr_act);
				refd_thr_act = THR_ACT_NULL;
			}
			if (thread &&
				thr_act == thread->top_act && thread != cur_thr) {
				refd_thr_act = thr_act;
				act_locked_act_reference(thr_act);
				act_unlock_thread(thr_act);
				task_unlock(task);
				(void)thread_wait(thread);
				task_lock(task);
				thread = act_lock_thread(thr_act);
				if (!thr_act->active) {
					act_unlock_thread(thr_act);
					break;
				}
			}
			act_unlock_thread(thr_act);
			thr_act = (thread_act_t) queue_next(&thr_act->thr_acts);
		}
	    	if (queue_end(list, (queue_entry_t)thr_act))
			break;
	}
	if (refd_thr_act != THR_ACT_NULL) {
		act_deallocate(refd_thr_act);
		refd_thr_act = THR_ACT_NULL;
	}
#endif
}

kern_return_t 
task_resume(register task_t task)
{
	register boolean_t	release;

	if (task == TASK_NULL)
		return(KERN_INVALID_ARGUMENT);

	release = FALSE;
#if 0	
	task_lock(task);
	if (!task->active) {
		task_unlock(task);
		return(KERN_FAILURE);
	}
	if (task->user_stop_count > 0) {
		if (--(task->user_stop_count) == 0)
	    		release = TRUE;
	}
	else {
		task_unlock(task);
		return(KERN_FAILURE);
	}
	task_unlock(task);
#endif
	/*
	 *	Release the task if necessary.
	 */
	if (release)
		return(task_release(task));

	return(KERN_SUCCESS);
}

kern_return_t
task_set_info(
	task_t		task,
	task_flavor_t	flavor,
	task_info_t	task_info_in __unused,		/* pointer to IN array */
	mach_msg_type_number_t	task_info_count __unused)
{

	if (task == TASK_NULL)
		return(KERN_INVALID_ARGUMENT);

	switch (flavor) {
	    default:
			return (KERN_INVALID_ARGUMENT);
	}
	return (KERN_SUCCESS);
}

kern_return_t
task_info(
	task_t			task,
	task_flavor_t		flavor,
	task_info_t		task_info_out,
	mach_msg_type_number_t	*task_info_count)
{

	if (task == TASK_NULL)
		return(KERN_INVALID_ARGUMENT);

	switch (flavor) {
	    case TASK_BASIC_INFO:
	    {
#ifdef notyet
		register task_basic_info_t	basic_info;
#endif

		if (*task_info_count < TASK_BASIC_INFO_COUNT) {
		    return(KERN_INVALID_ARGUMENT);
		}

#ifdef notyet
		basic_info = (task_basic_info_t) task_info_out;

		map = (task == kernel_task) ? kernel_map : task->map;

		basic_info->virtual_size  = map->size;
		basic_info->resident_size = pmap_resident_count(map->pmap)
						   * PAGE_SIZE;
		task_lock(task);
		basic_info->policy = task->policy;
		basic_info->suspend_count = task->user_stop_count;
		basic_info->user_time.seconds
				= task->total_user_time.seconds;
		basic_info->user_time.microseconds
				= task->total_user_time.microseconds;
		basic_info->system_time.seconds
				= task->total_system_time.seconds;
		basic_info->system_time.microseconds 
				= task->total_system_time.microseconds;
		task_unlock(task);
#endif

		*task_info_count = TASK_BASIC_INFO_COUNT;
		break;
	    }

	    case TASK_THREAD_TIMES_INFO:
	    {
		register task_thread_times_info_t times_info;

		if (*task_info_count < TASK_THREAD_TIMES_INFO_COUNT) {
		    return (KERN_INVALID_ARGUMENT);
		}

		times_info = (task_thread_times_info_t) task_info_out;
		times_info->user_time.seconds = 0;
		times_info->user_time.microseconds = 0;
		times_info->system_time.seconds = 0;
		times_info->system_time.microseconds = 0;

#ifdef notyet
		task_lock(task);
		queue_iterate(&task->thr_acts, thr_act,
			      thread_act_t, thr_acts)
		{
			thread_t thread;
		    time_value_t user_time, system_time;
		    spl_t	 s;

		    thread = act_lock_thread(thr_act);

		    /* Skip empty threads and threads that have migrated
		     * into this task:
		     */
		    if (thr_act->ith_object) {
				act_unlock_thread(thr_act);
				continue;
		    }
		    assert(thread);	/* Must have thread, if no thread_pool*/
		    s = splsched();
		    thread_lock(thread);

		    thread_read_times(thread, &user_time, &system_time);

		    thread_unlock(thread);
		    splx(s);
		    act_unlock_thread(thr_act);

		    time_value_add(&times_info->user_time, &user_time);
		    time_value_add(&times_info->system_time, &system_time);
		}
		task_unlock(task);
#endif
		*task_info_count = TASK_THREAD_TIMES_INFO_COUNT;
		break;
	    }

	    case TASK_SCHED_FIFO_INFO:
	    {
		register policy_fifo_base_t	fifo_base;

		if (*task_info_count < POLICY_FIFO_BASE_COUNT)
			return(KERN_INVALID_ARGUMENT);

		fifo_base = (policy_fifo_base_t) task_info_out;

		task_lock(task);
		if (task->policy != POLICY_FIFO) {
			task_unlock(task);
			return(KERN_INVALID_POLICY);
		}
		fifo_base->base_priority = task->priority;
		task_unlock(task);

		*task_info_count = POLICY_FIFO_BASE_COUNT;
		break;
	    }

	    case TASK_SCHED_RR_INFO:
	    {
		register policy_rr_base_t	rr_base;

		if (*task_info_count < POLICY_RR_BASE_COUNT)
			return(KERN_INVALID_ARGUMENT);

		rr_base = (policy_rr_base_t) task_info_out;

		task_lock(task);
		if (task->policy != POLICY_RR) {
			task_unlock(task);
			return(KERN_INVALID_POLICY);
		}
		rr_base->base_priority = task->priority;
		rr_base->quantum = (task->sched_data * ticks)/1000;
		task_unlock(task);

		*task_info_count = POLICY_RR_BASE_COUNT;
		break;
	    }

	    case TASK_SCHED_TIMESHARE_INFO:
	    {
		register policy_timeshare_base_t	ts_base;

		if (*task_info_count < POLICY_TIMESHARE_BASE_COUNT)
			return(KERN_INVALID_ARGUMENT);

		ts_base = (policy_timeshare_base_t) task_info_out;

		task_lock(task);
		if (task->policy != POLICY_TIMESHARE) {
			task_unlock(task);
			return(KERN_INVALID_POLICY);
		}
		ts_base->base_priority = task->priority;
		task_unlock(task);

		*task_info_count = POLICY_TIMESHARE_BASE_COUNT;
		break;
	    }

            case TASK_SECURITY_TOKEN:
	    {
                register security_token_t	*sec_token_p;

		if (*task_info_count < TASK_SECURITY_TOKEN_COUNT) {
		    return(KERN_INVALID_ARGUMENT);
		}

		sec_token_p = (security_token_t *) task_info_out;

		task_lock(task);
		*sec_token_p = task->sec_token;
		task_unlock(task);

		*task_info_count = TASK_SECURITY_TOKEN_COUNT;
                break;
            }
            
	    default:
		return (KERN_INVALID_ARGUMENT);
	}

	return(KERN_SUCCESS);
}

/*
 *	task_assign:
 *
 *	Change the assigned processor set for the task
 */
kern_return_t
task_assign(
	task_t				task __unused,
	processor_set_t			new_pset __unused,
	boolean_t			assign_threads __unused)
{

	return (KERN_FAILURE);
}

/*
 *	task_assign_default:
 *
 *	Version of task_assign to assign to default processor set.
 */
kern_return_t
task_assign_default(
	task_t		task,
	boolean_t	assign_threads)
{
    return (task_assign(task, &default_pset, assign_threads));
}

/*
 *	task_get_assignment
 *
 *	Return name of processor set that task is assigned to.
 */
kern_return_t
task_get_assignment(
	task_t		task,
	processor_set_t	*pset)
{
	if (!task->active)
		return(KERN_FAILURE);

	*pset = task->processor_set;
	pset_reference(*pset);
	return(KERN_SUCCESS);
}

/*
 * 	task_policy
 *
 *	Set scheduling policy and parameters, both base and limit, for
 *	the given task. Policy must be a policy which is enabled for the
 *	processor set. Change contained threads if requested. 
 */
kern_return_t
task_policy(
	task_t			task,
        policy_t		policy,
        policy_base_t		base,
	mach_msg_type_number_t	count,
        boolean_t		set_limit,
        boolean_t		change)
{

	return (KERN_FAILURE);
}

kern_return_t
task_set_policy(
	task_t			task __unused,
	processor_set_t		pset __unused,
	policy_t		policy __unused,
	policy_base_t		base __unused,
	mach_msg_type_number_t	base_count __unused,
	policy_limit_t		limit __unused,
	mach_msg_type_number_t	limit_count __unused,
	boolean_t		change __unused)
{

	return (KERN_FAILURE);
}


kern_return_t
task_set_ras_pc(
 	task_t		task __unused,
 	vm_offset_t	pc __unused,
 	vm_offset_t	endpc __unused)
{

	return (KERN_FAILURE);
}

void
task_synchronizer_destroy_all(task_t task)
{
	semaphore_t	semaphore;

	/*
	 *  Destroy owned semaphores
	 */

	while (!queue_empty(&task->semaphore_list)) {
		semaphore = (semaphore_t) queue_first(&task->semaphore_list);
		(void) semaphore_destroy(task, semaphore);
	}
}

static long task_uniqueid;

static void
mach_task_init(void *arg __unused, struct proc *p)
{
	task_t task;

	p->p_machdata = task = uma_zalloc(task_zone, M_WAITOK|M_ZERO);
	task->itk_p = p;

	mach_mutex_init(&task->lock, "ETAP_THREAD_TASK_NEW");
	mach_mutex_init(&task->itk_lock_data, "ETAP_THREAD_TASK_ITK");
	queue_init(&task->semaphore_list);

	if (p == &proc0) {
		kernel_task = task;
		task_create_internal(task);
		task_init_internal(TASK_NULL, task);
	} else {
		task_create_internal(task);
	}
}

/*
 * mach_task_init_lazy — give a process a Mach task on demand.
 *
 * Called by the wired Mach syscall wrappers when p_machdata == NULL —
 * i.e. for any process that pre-existed mach.ko's load (the eventhandler
 * couldn't have run for them) or any process whose Mach state was torn
 * down. Idempotent: if p_machdata is already set, returns 0 immediately.
 *
 * Concurrency: two threads in the same process racing through the
 * wrappers will both reach here with p_machdata == NULL. Both allocate
 * a task; the loser's task gets discarded via the atomic_cmpset_ptr
 * check below. The loser-task leak is small (one struct mach_task) and
 * bounded (at most one per losing race), and the alternative — taking
 * a sleepable lock around uma_zalloc — is heavier.
 *
 * Returns 0 on success, non-zero on allocation failure.
 */
int
mach_task_init_lazy(struct proc *p)
{
	task_t task;

	if (p->p_machdata != NULL)
		return (0);

	task = uma_zalloc(task_zone, M_NOWAIT | M_ZERO);
	if (task == NULL)
		return (ENOMEM);
	task->itk_p = p;
	mach_mutex_init(&task->lock, "ETAP_THREAD_TASK_NEW");
	mach_mutex_init(&task->itk_lock_data, "ETAP_THREAD_TASK_ITK");
	queue_init(&task->semaphore_list);
	task_create_internal(task);
	task_init_internal(TASK_NULL, task);
	/* Task #39: same audit_token fix as mach_task_fork_bsport — without
	 * this, lazy-initialized tasks (pre-existing processes that came up
	 * before mach.ko loaded) carry KERNEL_AUDIT_TOKEN, breaking any
	 * MIG call whose handler keys on audit_token.val[5]. */
	set_security_token(task);

	if (atomic_cmpset_ptr((volatile uintptr_t *)&p->p_machdata,
	    (uintptr_t)NULL, (uintptr_t)task))
		return (0);

	/*
	 * Lost the race — another thread installed a task first. Leak
	 * our task; teardown of an unreachable task_create_internal-style
	 * object is non-trivial and the leak is bounded.
	 */
	return (0);
}

static void
mach_task_fork(void *arg __unused, struct proc *p1, struct proc *p2, int flags __unused)
{
	task_t task;
	task_t parent_task;

	/*
	 * Out-of-tree fix: we still need to initialize the child task
	 * even when the parent has no Mach state (the parent existed
	 * before mach.ko loaded — common for any process descended from
	 * pre-load init/sshd). Without this initialization, the child's
	 * p_machdata points at a zero-filled task struct (allocated by
	 * mach_task_init's process_init handler) and any subsequent
	 * deref of task->itk_space NULL-faults.
	 *
	 * - If the child has no task (shouldn't happen post-mach.ko, but
	 *   defensive): early-return.
	 * - If parent has no Mach state: initialize child with TASK_NULL
	 *   parent. task_init_internal handles NULL parent_task by
	 *   skipping the inheritance step.
	 * - Otherwise: full parent-derived init.
	 */
	task = p2->p_machdata;
	if (task == NULL)
		return;

	parent_task = p1->p_machdata;
	if (parent_task == NULL) {
		task_init_internal(TASK_NULL, task);
		return;
	}

	atomic_add_long(&task_uniqueid, 1);
	task->itk_uniqueid = task_uniqueid;
	task->itk_puniqueid = parent_task->itk_uniqueid;
	task_init_internal(parent_task, task);
}

static int
uma_task_init(void *_thread, int a, int b)
{
	/* allocate task substructures */
	return (0);
}

static void
uma_task_fini(void *_thread, int a)
{
	/* deallocate task substructures */
}


/*
 * Task #39 fix: minimal fork-time bootstrap port inheritance.
 *
 * Apple's model: fork() copies parent's task port state (including
 * itk_bootstrap) into child. Our Phase C2 disabled the full
 * task-init / task-fork eventhandlers because they panicked in
 * thread_self_trap when only task (not thread) eventhandlers were
 * wired — full restoration needs both layers safe.
 *
 * But the daemons launchd spawns *must* see launchd's bootstrap_port
 * inherited at fork time. By the time syslogd's lazy mach_task_init
 * runs (via task_self_trap), launchd has already reset its own
 * bootstrap port back to NULL after the fork. So lazy-side "look at
 * parent" inheritance won't work — the snapshot must happen at fork.
 *
 * This handler does the bare minimum: at fork, if the parent has a
 * non-NULL itk_bootstrap, give the child a fresh task with that
 * bootstrap port pre-set. Does NOT call task_init_internal (which is
 * what panicked in 2026-05-11). Threads are still lazy.
 */
static eventhandler_tag mach_fork_eh_tag;

static void
mach_task_fork_bsport(void *arg __unused, struct proc *p1, struct proc *p2,
    int flags __unused)
{
	task_t parent_task = p1->p_machdata;
	task_t child_task;

	if (mach_debug_enable)
		printf("[T39-fork] p1=%s(%d) -> p2=%s(%d) parent_task=%p "
		    "itk_bootstrap=%p\n",
		    p1->p_comm, p1->p_pid, p2->p_comm, p2->p_pid,
		    parent_task,
		    parent_task ? (void *)parent_task->itk_bootstrap : NULL);

	if (parent_task == NULL)
		return; /* parent has no Mach state — nothing to inherit */
	if (parent_task->itk_bootstrap == IP_NULL)
		return; /* parent has no bootstrap port — nothing to inherit */

	/* Child should have p_machdata set up the same way mach_task_init_lazy
	 * does on first syscall. We pre-create it here so the bootstrap port
	 * is available immediately. If the child already has a task (race),
	 * use the existing one. */
	if (p2->p_machdata == NULL) {
		child_task = uma_zalloc(task_zone, M_NOWAIT | M_ZERO);
		if (child_task == NULL)
			return; /* OOM; fall back to no inheritance */
		child_task->itk_p = p2;
		mach_mutex_init(&child_task->lock, "ETAP_THREAD_TASK_NEW");
		mach_mutex_init(&child_task->itk_lock_data, "ETAP_THREAD_TASK_ITK");
		queue_init(&child_task->semaphore_list);
		task_create_internal(child_task);
		/* TASK_NULL parent — full inheritance is what tripped the
		 * earlier panic. Just init the empty task, then patch in
		 * the bootstrap port below. */
		task_init_internal(TASK_NULL, child_task);

		if (!atomic_cmpset_ptr((volatile uintptr_t *)&p2->p_machdata,
		    (uintptr_t)NULL, (uintptr_t)child_task)) {
			/* lost race; leak our task, fall through to use the
			 * winner */
			child_task = p2->p_machdata;
		}
	} else {
		child_task = p2->p_machdata;
	}

	/* Task #39: task_init_internal(TASK_NULL, ...) above leaves
	 * audit_token = KERNEL_AUDIT_TOKEN (val[5]=0). Without this,
	 * launchd's job_mig_intran looks up the caller by PID=0, finds
	 * nothing, falls through to job_new_anonymous(jm, 0) which
	 * short-circuits and returns NULL — so job_mig_check_in2 sees
	 * j=NULL and returns BOOTSTRAP_NO_MEMORY. Stamp the real PID
	 * and ucred into the child's audit_token now. */
	set_security_token(child_task);

	itk_lock(parent_task);
	itk_lock(child_task);

	/* Replace whatever the child's itk_bootstrap was (probably IP_NULL
	 * from task_init_internal's TASK_NULL-parent branch) with a send
	 * right to the parent's bootstrap port. */
	if (IP_VALID(child_task->itk_bootstrap))
		ipc_port_release_send(child_task->itk_bootstrap);
	child_task->itk_bootstrap = ipc_port_copy_send(parent_task->itk_bootstrap);

	if (mach_debug_enable)
		printf("[T39-fork] %s(%d) inherited bsport %p -> child_task=%p\n",
		    p2->p_comm, p2->p_pid,
		    (void *)child_task->itk_bootstrap, child_task);

	itk_unlock(child_task);
	itk_unlock(parent_task);
}

static void
task_sysinit(void *arg __unused)
{
	task_zone = uma_zcreate("mach_task_zone",
							sizeof(struct mach_task),
							NULL, NULL, uma_task_init,
							uma_task_fini, 1, 0);

	/*
	 * Phase C2 (still in effect): do NOT re-enable the full
	 * process_init or process_fork eventhandlers — they panicked in
	 * thread_self_trap when only task (not thread) state was prepped
	 * at fork time. Lazy init is the baseline.
	 *
	 * Task #39 (2026-05-18): register a MINIMAL fork eventhandler that
	 * does ONLY bootstrap port inheritance. This is the smallest patch
	 * that lets launchd-spawned daemons see launchd's bootstrap_port
	 * via task_get_special_port at startup — which they need to call
	 * bootstrap_check_in for their MachServices.
	 */
	mach_fork_eh_tag = EVENTHANDLER_REGISTER(process_fork,
	    mach_task_fork_bsport, NULL, EVENTHANDLER_PRI_ANY);
}

/* before SI_SUB_INTRINSIC and after SI_SUB_EVENTHANDLER */
SYSINIT(mach_thread, SI_SUB_KLD, SI_ORDER_ANY, task_sysinit, NULL);
