/*
 * mach/clock.h — Apple-canonical Mach clock service umbrella.
 *
 * On Apple this is the public header that declares clock_get_time,
 * clock_set_time, clock_alarm, clock_sleep_trap, etc., over the
 * <mach/clock_types.h> type set. Apple-source consumers (libdispatch's
 * internal.h includes it unconditionally on HAVE_MACH paths) reference
 * the header even when they don't call its functions — they typically
 * just want mach_timespec_t from <mach/clock_types.h>.
 *
 * On freebsd-launchd-mach we don't ship a Mach clock service; this
 * header is a thin re-export of clock_types.h so #include <mach/clock.h>
 * resolves and mach_timespec_t is in scope. Add real prototypes /
 * RPC stubs only when a real consumer needs them.
 */
#ifndef _MACH_CLOCK_H_
#define _MACH_CLOCK_H_

#include <mach/clock_types.h>

#endif /* !_MACH_CLOCK_H_ */
