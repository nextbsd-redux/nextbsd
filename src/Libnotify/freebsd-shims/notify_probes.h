/* notify_probes.h — FreeBSD shim. Apple generates this from a .d
 * (DTrace) file via `dtrace -h`. The generated macro names follow
 * dtrace's PROVIDER_PROBE convention: provider `notify`, probe
 * `post` -> macro NOTIFY_POST(...), plus a NOTIFY_POST_ENABLED()
 * predicate. Stub every probe libnotify's sources reference to a
 * no-op so they build/link without dtrace.
 *
 * Names below must match the call sites verbatim (libnotify.c,
 * notify_client.c, table.c) — a mismatch compiles fine but leaves
 * an undefined symbol that only fails at runtime link. */
#ifndef _FREEBSD_SHIM_NOTIFY_PROBES_H_
#define _FREEBSD_SHIM_NOTIFY_PROBES_H_

#define NOTIFY_POST(...)			do {} while (0)
#define NOTIFY_POST_ENABLED()			0
#define NOTIFY_CHECK(...)			do {} while (0)
#define NOTIFY_CHECK_ENABLED()			0
#define NOTIFY_REGISTER_MACH_PORT(...)		do {} while (0)
#define NOTIFY_REGISTER_MACH_PORT_ENABLED()	0
#define NOTIFY_DELIVER_START(...)		do {} while (0)
#define NOTIFY_DELIVER_START_ENABLED()		0
#define NOTIFY_DELIVER_END(...)			do {} while (0)
#define NOTIFY_DELIVER_END_ENABLED()		0

#endif
