/* os/reason_private.h — FreeBSD shim. Apple's os_reason API for
 * recording exit-reason metadata that surfaces in CrashReporter.
 * Stub to no-ops since we don't have CrashReporter. */
#ifndef _FREEBSD_SHIM_OS_REASON_PRIVATE_H_
#define _FREEBSD_SHIM_OS_REASON_PRIVATE_H_

#include <stdint.h>

#define OS_REASON_FLAG_NONE			0

#define os_fault_with_payload(ns, code, payload, sz, fmt, ...) (void)0
#define abort_with_reason(ns, code, fmt, flags)	abort()
#define abort_with_payload(ns, code, payload, sz, fmt, flags) abort()

#endif
