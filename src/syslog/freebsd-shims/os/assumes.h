/* os/assumes.h — FreeBSD shim. Apple's "soft assert" macros that log
 * but don't abort. Stub to assert(3) for now. */
#ifndef _FREEBSD_SHIM_OS_ASSUMES_H_
#define _FREEBSD_SHIM_OS_ASSUMES_H_

#include <assert.h>

#define os_assumes(expr)		((expr) ? 1 : (assert(expr), 0))
#define os_assumes_zero(expr)		((expr) == 0 ? 0 : ((expr)))
#define os_assert(expr)			assert(expr)
#define os_assert_zero(expr)		assert((expr) == 0)

#define os_unlikely(expr)		__builtin_expect(!!(expr), 0)
#define os_likely(expr)			__builtin_expect(!!(expr), 1)

#endif
