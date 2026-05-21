# hwregd — freebsd-launchd-mach hardware registry daemon.
#
# Phase 0 / iter 1: minimum viable /dev/devctl reader. Builds to
# /usr/sbin/hwregd, runs as a launchd-managed daemon under PID-1
# launchd (plist at /System/Library/LaunchDaemons/org.freebsd.hwregd.plist).
#
# iter 3b links Mach IPC (-llaunch / -lsystem_kernel) for the
# org.freebsd.hwregd pub/sub service; pass SYSROOT to point the
# linker at the rootfs's /usr/lib/system.
#
# Build:   cd src/hwregd && make SYSROOT=/path/to/rootfs
# Install: make DESTDIR=/path/to/rootfs install

PROG=		hwregd
MAN=
# hwreg_registry.c — Phase 1 hardware registry tree (newbus snapshot
# via hw.bus.* sysctls + devctl-event maintenance). libc-only, no
# extra LDADD.
SRCS=		hwregd.c hwreg_registry.c

# hwregServer.c — MIG-generated server stub for hwreg.defs (Phase 1
# iter 2a Mach-RPC query API). build.sh runs mig and passes MIGOUT=;
# a bare standalone build without it fails at the generated #include,
# the same way liblaunch does. -w on the generated TU: MIG output is
# not WARNS-clean and must not gate the build.
MIGOUT?=
.if !empty(MIGOUT)
.PATH:			${MIGOUT}
SRCS+=			hwregServer.c
CFLAGS+=		-I${MIGOUT}
CFLAGS+=		-I${.CURDIR}	# generated stubs #include "hwreg_mig_types.h"
CFLAGS.hwregServer.c+=	-w
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

# iter 3b: Mach IPC for the org.freebsd.hwregd pub/sub service.
# <servers/bootstrap.h> (bootstrap_check_in + the bootstrap_port
# global) comes from liblaunch; its bootstrap.h pulls in Apple
# shim headers (AvailabilityMacros.h, ...) from launchd/freebsd-shims;
# the Mach headers come from the rootfs.
CFLAGS+=	-I${.CURDIR}/../launchd/liblaunch
CFLAGS+=	-I${.CURDIR}/../launchd/freebsd-shims
# iter 2b: nv.h — the libxpc nvlist API hwregd packs property bags with.
CFLAGS+=	-I${.CURDIR}/../libxpc

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

# -llaunch: bootstrap_check_in + the bootstrap_port global.
# -lsystem_kernel: mach_msg and the Mach port syscalls.
# -lxpc: the nvlist API (nvlist_pack / _unpack / ...) for property bags.
LDADD+=		-llaunch -lsystem_kernel -lpthread -lxpc

BINDIR=		/usr/sbin

.include <bsd.prog.mk>
