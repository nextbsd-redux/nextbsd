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
 * @APPLE_LICENSE_HEADER_END@
 */

/*
 * SystemConfiguration.h — umbrella header for the SystemConfiguration
 * client framework (freebsd-launchd-mach port).
 *
 * Exposes the SCDynamicStore, SCPreferences, SCNetworkConfiguration,
 * and SCNetworkReachability APIs, plus the SCSchemaDefinitions
 * constants. The remaining network APIs (Connection, ...) are later
 * iterations.
 */

#ifndef _SYSTEMCONFIGURATION_H
#define _SYSTEMCONFIGURATION_H

#include <SystemConfiguration/SCDynamicStore.h>
#include <SystemConfiguration/SCPreferences.h>
#include <SystemConfiguration/SCNetworkConfiguration.h>
#include <SystemConfiguration/SCNetworkReachability.h>
#include <SystemConfiguration/SCSchemaDefinitions.h>

#endif	/* _SYSTEMCONFIGURATION_H */
