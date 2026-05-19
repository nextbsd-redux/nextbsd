/*
 * mach/sync_policy.h — Apple Mach semaphore policy constants.
 *
 * Used by libdispatch's shims/lock.h on the USE_MACH_SEM path to
 * configure `semaphore_create` ordering. Our semaphore_create
 * implementation in libmach maps these to underlying primitives;
 * the constant values match Apple's <mach/sync_policy.h>.
 */
#ifndef _MACH_SYNC_POLICY_H_
#define _MACH_SYNC_POLICY_H_

#define SYNC_POLICY_FIFO		0x0
#define SYNC_POLICY_FIXED_PRIORITY	0x1
#define SYNC_POLICY_REVERSED		0x2
#define SYNC_POLICY_ORDER_MASK		0x3
#define SYNC_POLICY_LIFO		(SYNC_POLICY_FIFO | SYNC_POLICY_REVERSED)

#define SYNC_POLICY_PREPOST		0x4
#define SYNC_POLICY_MAX			0x7

#endif /* !_MACH_SYNC_POLICY_H_ */
