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
SRCS=		diskarbitrationd.c da_iokit_subscribe.c

NO_MAN=		yes

# WARNS=0: da_iokit_subscribe.c pulls <IOKit/IOKitLib.h>, which drags in the
# whole CoreFoundation + dispatch public header tree (the IONotificationPort /
# IOServiceMatchingCallback / CFDictionary surface). At WARNS>0 the FreeBSD
# warning machinery adds -Wsystem-headers -Werror, which promotes warnings
# *inside* the vendored CF headers (CFLocking.h's "CF is not thread-safe"
# #warning, -Wexpansion-to-defined) into hard errors. libIOKit + its test
# clients + kext_tools all build at WARNS=0 for exactly this reason. The other
# two DA sources (diskarbitrationd.c, hwreg_subscribe.c) still get the explicit
# warning set below.
WARNS=		0

CFLAGS+=	-O2 -pipe
CFLAGS+=	-Wall -Wextra -Wshadow -Wstrict-prototypes
CFLAGS+=	-Wmissing-prototypes -Wpointer-arith

# -fblocks: <IOKit/IOKitLib.h> pulls CoreFoundation + <dispatch/dispatch.h>,
# whose headers carry block-typed function pointers that need -fblocks even
# where DA itself uses no blocks. Same flag libIOKit + its clients carry.
CFLAGS+=	-fblocks

# ${SYSROOT}/usr/include MUST precede freebsd-shims: <IOKit/IOKitLib.h> has
# to resolve to libIOKit's REAL installed header (IONotificationPortRef /
# io_iterator_t / io_name_t / IOServiceAddMatchingNotification, which
# da_iokit_subscribe.c needs) and NOT the freebsd-shims IOKit stub
# (IOKitWaitQuiet-only, for launchctl). IOKitLib.h is the only header present
# in both trees; AvailabilityMacros.h / NSSystemDirectories.h etc. are
# Apple-only and still resolve from freebsd-shims regardless of order.
SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
.endif

# <servers/bootstrap.h> (bootstrap_check_in + bootstrap_port global)
# comes from liblaunch; its bootstrap.h pulls in Apple shim headers
# (AvailabilityMacros.h, ...) from launchd/freebsd-shims. Same -I
# pair hwregd / configd / ipconfigd / mDNSResponder use.
CFLAGS+=	-I${.CURDIR}/../launchd/liblaunch
CFLAGS+=	-I${.CURDIR}/../launchd/freebsd-shims

.if !empty(SYSROOT)
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

# -llaunch: bootstrap_check_in + the bootstrap_port global.
# -lsystem_kernel: mach_msg + the Mach port syscalls behind it.
# -lpthread: libIOKit's IONotificationPort receive loop (linked transitively).
# -lIOKit: the IONotificationPort / IOServiceAddMatchingNotification kernel-
#   notify path (C1.3, #218). -lCoreFoundation: the CFDictionary matching-dict
#   object model libIOKit's API consumes. -ldispatch + -lBlocksRuntime: the
#   dispatch queue DA delivers notification callbacks on
#   (IONotificationPortSetDispatchQueue) + the -fblocks runtime CF/dispatch pull.
LDADD+=		-llaunch -lsystem_kernel -lpthread
LDADD+=		-lIOKit -lCoreFoundation -ldispatch -lBlocksRuntime

BINDIR=		/usr/sbin

.include <bsd.prog.mk>
