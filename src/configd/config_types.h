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
 * Input arguments: serialized key's, list delimiters, ...
 *	(sent as out-of-line data in a message)
 */
typedef const void * xmlData_t;

/* Output arguments: serialized data, lists, ...
 *	(sent as out-of-line data in a message)
 */
typedef const void * xmlDataOut_t;

#endif	/* !_CONFIG_TYPES_H */
