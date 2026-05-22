/*
 * Copyright (c) 2000-2022 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

/*
 * config_types.h — types and constants imported into config.defs and
 * the MIG-generated config{User,Server}.{c,h} stubs.
 *
 * freebsd-launchd-mach port: Apple's config_types.h gates on
 * <TargetConditionals.h> for the iOS-simulator service name and uses
 * `__private_extern__` for mig_external. This port has no simulator
 * variant and builds configd as a plain daemon, so the service name
 * is defined unconditionally and mig_external is plain `extern`.
 */

#ifndef _CONFIG_TYPES_H
#define _CONFIG_TYPES_H

#include <stdint.h>	/* uint8_t — config.defs' config_byte_t element ctype */

/*
 * Keep the MIG IPC functions at plain external linkage.
 */
#ifdef mig_external
#undef mig_external
#endif
#define mig_external extern

/* Turn MIG type checking on by default */
#ifdef __MigTypeCheck
#undef __MigTypeCheck
#endif
#define __MigTypeCheck	1

/*
 * Mach server port name — the bootstrap service configd checks in and
 * the SystemConfiguration framework looks up.
 */
#define SCD_SERVER	"com.apple.SystemConfiguration.configd"

/*
 * SCDynamicStore status codes — a subset of Apple's SCError.h. configd
 * and its clients must agree on these values: they cross the wire as
 * the `status` reply field of every config.defs routine.
 */
#define kSCStatusOK			0	/* success */
#define kSCStatusFailed			1001	/* non-specific failure */
#define kSCStatusInvalidArgument	1002	/* invalid argument */
#define kSCStatusNoKey			1004	/* no such key */
#define kSCStatusKeyExists		1005	/* key already defined */
#define kSCStatusNoStoreSession		2001	/* no open store session */
#define kSCStatusNotifierActive		2003	/* notifier already active */

/* Maximum key / value size — config.defs' array[*:8192] bound. */
#define CONFIG_DATA_MAX			8192

/*
 * config.defs' xmlData / xmlDataOut MIG types. MIG records a `type`
 * declaration's wire layout but emits no C typedef for it — the
 * generated stubs use the name as-is and expect this imported header
 * to define it (cf. hwregd's hwreg_mig_types.h). The bound matches
 * config.defs' array[*:8192]; the payload is carried inline in the
 * message, so the type is the byte array itself, not a pointer.
 */
typedef uint8_t xmlData[8192];
typedef uint8_t xmlDataOut[8192];

#endif	/* !_CONFIG_TYPES_H */
