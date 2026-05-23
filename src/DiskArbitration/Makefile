# diskarbitrationd — freebsd-launchd-mach DiskArbitration daemon.
#
# iter 1: daemon skeleton. main + bootstrap_check_in for
# com.apple.DiskArbitration + a sleep loop. Same shape as
# src/IPConfiguration/Makefile (iter 1 state) and src/hwregd/Makefile
# / src/mDNSResponder/Makefile (the iter-1 mDNS shell).
#
# iter 2+ adds hwregd subscription (Mach RPC), libgeom enrichment,
# and the da.defs MIG IDL serving DiskArbitration.framework clients.
#
# Build:   cd src/DiskArbitration && make SYSROOT=/path/to/rootfs
# Install: make DESTDIR=/path/to/rootfs install

PROG=		diskarbitrationd
MAN=
SRCS=		diskarbitrationd.c hwreg_subscribe.c

NO_MAN=		yes
WARNS=		6

CFLAGS+=	-O2 -pipe
CFLAGS+=	-Wall -Wextra -Wshadow -Wstrict-prototypes
CFLAGS+=	-Wmissing-prototypes -Wpointer-arith

# <servers/bootstrap.h> (bootstrap_check_in + bootstrap_port global)
# comes from liblaunch; its bootstrap.h pulls in Apple shim headers
# (AvailabilityMacros.h, ...) from launchd/freebsd-shims. Same -I
# pair hwregd / configd / ipconfigd / mDNSResponder use.
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
# -lpthread: the hwregd subscription receive-loop runs on its own thread.
LDADD+=		-llaunch -lsystem_kernel -lpthread

BINDIR=		/usr/sbin

.include <bsd.prog.mk>
