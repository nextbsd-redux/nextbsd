# configd — SCDynamicStore configuration daemon (freebsd-launchd-mach).
#
# configd builds to /usr/sbin/configd and runs as a launchd-managed
# daemon under PID-1 launchd (plist at /System/Library/LaunchDaemons/
# com.apple.configd.plist). It checks the
# com.apple.SystemConfiguration.configd Mach service in and runs a raw
# mach_msg loop over the MIG config.defs demux (config_server()).
# iter 2 adds config_store.c — the SCDynamicStore key/value store
# backing configopen / configget / configset / configremove. iter 4
# adds config_session.c — per-session ports, key watches and the
# change-notification fan-out backing notifyadd / notifyviaport /
# notifychanges. iter 7 adds config_wire.c — the key-list / key-value
# byte encodings the batch routines (notifyset / configget_m /
# configset_m) carry in their inline payloads.
#
# Build:   cd src/configd && make SYSROOT=/path/to/rootfs MIGOUT=...
# Install: make DESTDIR=/path/to/rootfs install

PROG=		configd
MAN=

SRCS=		configd.c config_store.c config_session.c config_wire.c

# configServer.c — MIG-generated server demux for config.defs.
# configUser.c — MIG-generated client stubs, linked for the in-process
# `configd --selftest` round trip. build.sh runs mig and passes
# MIGOUT=; a bare standalone build without it fails at the generated
# #include "configServer.h", which is expected — configd cannot build
# without the MIG output.
MIGOUT?=
.if !empty(MIGOUT)
.PATH:			${MIGOUT}
SRCS+=			configServer.c configUser.c
CFLAGS+=		-I${MIGOUT}
CFLAGS+=		-I${.CURDIR}	# generated stubs #include "config_types.h"
CFLAGS.configServer.c+=	-w
CFLAGS.configUser.c+=	-w
# <mach/notify.h> (pulled by the MIG stubs and, for the no-senders
# notification, by configd.c / config_session.c) redefines the
# MACH_NOTIFY_* macros <mach/message.h> also defines (identical values,
# whitespace differs) — a benign clash in the vendored mach headers.
CFLAGS+=		-Wno-macro-redefined
.endif

WARNS?=		6
NO_MAN=		yes

CFLAGS+=	-O2 -pipe
CFLAGS+=	-Wall -Wextra -Wshadow -Wstrict-prototypes
CFLAGS+=	-Wmissing-prototypes -Wpointer-arith

# <servers/bootstrap.h> (bootstrap_check_in + the bootstrap_port
# global) comes from liblaunch; its bootstrap.h pulls in Apple shim
# headers (AvailabilityMacros.h, ...) from launchd/freebsd-shims.
CFLAGS+=	-I${.CURDIR}/../launchd/liblaunch
CFLAGS+=	-I${.CURDIR}/../launchd/freebsd-shims

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

# -llaunch: bootstrap_check_in + the bootstrap_port global.
# -lsystem_kernel: mach_msg and the Mach port syscalls.
LDADD+=		-llaunch -lsystem_kernel -lpthread

BINDIR=		/usr/sbin

.include <bsd.prog.mk>
