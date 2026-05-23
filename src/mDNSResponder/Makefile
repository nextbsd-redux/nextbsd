# mDNSResponder — freebsd-launchd-mach Bonjour/zeroconf daemon.
#
# iter 1: daemon skeleton. No vendored Apple mDNS sources yet — just
# proves the Mach service plumbing + launchd plist + build infra
# work. iter 2 imports mDNSCore + mDNSPosix + mDNSShared (~4 MB of
# Apple source) and wires the engine.
#
# Builds /usr/sbin/mDNSResponder; runs as a launchd-managed daemon
# under com.apple.mDNSResponder.plist in overlays/.
#
# Build:   cd src/mDNSResponder && make SYSROOT=/path/to/rootfs
# Install: make DESTDIR=/path/to/rootfs install

PROG=		mDNSResponder
MAN=
SRCS=		main.c

NO_MAN=		yes
WARNS=		6

CFLAGS+=	-O2 -pipe -fblocks
CFLAGS+=	-Wall -Wextra -Wshadow -Wstrict-prototypes
CFLAGS+=	-Wmissing-prototypes -Wpointer-arith

# <servers/bootstrap.h> (bootstrap_check_in + bootstrap_port global)
# comes from liblaunch; its bootstrap.h pulls in Apple shim headers
# (AvailabilityMacros.h, ...) from launchd/freebsd-shims. Same -I
# pair hwregd / configd / ipconfigd use.
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
# -lsystem_kernel: mach_msg + the Mach port syscalls behind it.
LDADD+=		-llaunch -lsystem_kernel

BINDIR=		/usr/sbin

.include <bsd.prog.mk>
