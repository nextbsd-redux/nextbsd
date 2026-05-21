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
SRCS=		hwregd.c

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
