/* os/variant_private.h — FreeBSD shim. Apple's variant API checks
 * whether the current OS variant has a feature enabled (internal
 * vs customer builds, etc.). For us: nothing is "internal," and
 * we don't gate behavior on this. Stub to "feature disabled." */
#ifndef _FREEBSD_SHIM_OS_VARIANT_PRIVATE_H_
#define _FREEBSD_SHIM_OS_VARIANT_PRIVATE_H_

#include <stdbool.h>

#define os_variant_has_internal_content(subsystem)	false
#define os_variant_has_internal_diagnostics(subsystem)	false
#define os_variant_has_internal_ui(subsystem)		false
#define os_variant_is_recovery(subsystem)		false
/* For FreeBSD port: returning TRUE makes asl_out_dst_set_access
 * (asl_common.c:870) short-circuit before the Darwin ACL setup
 * (acl_init, acl_create_entry_np, etc. — not portable to FreeBSD).
 * We are semantically a "basesystem" — no full Darwin userland. */
#define os_variant_is_basesystem(subsystem)		true
#define os_variant_check(subsystem, variant)		false

#endif
