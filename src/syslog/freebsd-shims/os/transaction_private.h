/* os/transaction_private.h — FreeBSD shim. Apple's os_transaction*
 * keeps launchd from idle-exiting a daemon mid-operation. Stub to
 * opaque sentinel; daemons that hold transactions still run, just
 * without the idle-exit gating semantic. */
#ifndef _FREEBSD_SHIM_OS_TRANSACTION_PRIVATE_H_
#define _FREEBSD_SHIM_OS_TRANSACTION_PRIVATE_H_

#include <stdint.h>

typedef void * os_transaction_t;

#define os_transaction_create(name)			((os_transaction_t)(uintptr_t)1)
#define os_transaction_needs_more_time(t, sec)		false
#define os_transaction_complete(t)			(void)0
#define os_transaction_retain(t)			((os_transaction_t)(uintptr_t)1)
#define os_transaction_release(t)			(void)0

#endif
