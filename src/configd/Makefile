# configd — SCDynamicStore configuration daemon (freebsd-launchd-mach).
#
# configd iter 1: the daemon skeleton. Builds to /usr/sbin/configd,
# runs as a launchd-managed daemon under PID-1 launchd (plist at
# /System/Library/LaunchDaemons/com.apple.configd.plist). It checks
# the com.apple.SystemConfiguration.configd Mach service in and runs
# a raw mach_msg loop over the MIG config.defs demux (config_server());
# the routine handlers are stubs until iter 2's store engine.
#
# Build:   cd src/configd && make SYSROOT=/path/to/rootfs MIGOUT=...
# Install: make DESTDIR=/path/to/rootfs install

PROG=		configd
MAN=

SRCS=		configd.c

# configServer.c — MIG-generated server demux + routine stubs for
# config.defs. build.sh runs mig and passes MIGOUT=; a bare standalone
# build without it fails at the generated #include "configServer.h",
# which is expected — configd cannot build without the MIG output.
MIGOUT?=
.if !empty(MIGOUT)
.PATH:			${MIGOUT}
SRCS+=			configServer.c
CFLAGS+=		-I${MIGOUT}
CFLAGS+=		-I${.CURDIR}	# generated stubs #include "config_types.h"
CFLAGS.configServer.c+=	-w
# The MIG stubs pull <mach/notify.h>, which redefines the MACH_NOTIFY_*
# macros <mach/message.h> also defines (identical values, whitespace
# differs) — a benign clash in the vendored mach headers.
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
