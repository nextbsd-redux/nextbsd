/*
 * Copyright (c) 2005 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_APACHE_LICENSE_HEADER_START@
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 * @APPLE_APACHE_LICENSE_HEADER_END@
 */

#ifndef __LAUNCHD_H__
#define __LAUNCHD_H__

#include <mach/mach.h>
#include <mach/port.h>
#include "launch.h"
#include "bootstrap.h"
#include "runtime.h"

struct kevent;
struct conncb;

extern bool pid1_magic;
extern bool launchd_shutting_down;
extern bool fake_launchd_shutting_down;
extern bool network_up;
extern FILE *launchd_console;
extern uid_t launchd_uid;

/*
 * Verbose diagnostic trace enable. Read once at launchd startup from
 * kenv "launchd_trace=1" (set at the FreeBSD loader prompt, survives
 * the kernel→PID-1 handoff). Off by default. CI enables via expect
 * in tests/boot-test.sh. Gates [T41-*] fprintf trace points across
 * launchd; the matching kernel-side gate is sysctl mach.debug_enable.
 */
extern bool launchd_trace_enabled;
#include <stdio.h>
#define LD_TRACE(fmt, ...) do {						\
	if (launchd_trace_enabled)					\
		fprintf(stderr, fmt "\n", ##__VA_ARGS__);		\
} while (0)

void launchd_SessionCreate(void);
void launchd_shutdown(void);

enum {
	LAUNCHD_PERSISTENT_STORE_DB,
	LAUNCHD_PERSISTENT_STORE_LOGS,
};
char *launchd_copy_persistent_store(int type, const char *file);

int _fd(int fd);

#endif /* __LAUNCHD_H__ */
