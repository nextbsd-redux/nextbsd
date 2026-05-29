/*
 * freebsd-launchd-mach hostnamed freebsd-shim.
 *
 * Re-export the SCValidation shim under the <SystemConfiguration/...>
 * include path Apple's set-hostname.c uses.
 */
#ifndef _FREEBSD_LAUNCHD_MACH_SC_VALIDATION_INDIRECT_H_
#define _FREEBSD_LAUNCHD_MACH_SC_VALIDATION_INDIRECT_H_
#include "../SCValidation.h"
#endif
