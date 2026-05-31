# KernelEventMonitor — freebsd-launchd-mach SystemConfiguration
# kernel-event monitor. Reads PF_ROUTE link-state changes and
# publishes State:/Network/Interface/<if>/Link into configd's
# SCDynamicStore. Standalone daemon (Apple runs it as a configd
# plugin; this repo's configd has no plugin loader and stock FreeBSD
# lacks EVFILT_MACHPORT — see the in-process-hosting kernel ticket).
#
# Build:   cd src/KernelEventMonitor && make SYSROOT=/path/to/rootfs
# Install: make DESTDIR=/path/to/rootfs install

PROG=		KernelEventMonitor
# MAN= (empty) suppresses bsd.prog.mk's default man build — same
# suppression hwregd / configd / ipconfigd use.
MAN=
SRCS=		kernel_event_monitor.c

# WARNS=0: the source pulls the CoreFoundation public header tree via
# the SC umbrella header. At WARNS>0 FreeBSD adds -Wsystem-headers
# -Werror which promotes warnings inside the vendored CF headers to
# hard errors. Same carry ipconfigd / libSystemConfiguration use.
WARNS=		0
NO_MAN=		yes

CFLAGS+=	-O2 -pipe -fblocks
CFLAGS+=	-Wall -Wextra -Wshadow -Wstrict-prototypes
CFLAGS+=	-Wmissing-prototypes -Wpointer-arith

# <servers/bootstrap.h> (bootstrap_port global, used by
# SCDynamicStoreCreate to reach configd) comes from liblaunch; its
# bootstrap.h pulls Apple shim headers from launchd/freebsd-shims.
# Same -I pair ipconfigd / hwregd / configd use.
CFLAGS+=	-I${.CURDIR}/../launchd/liblaunch
CFLAGS+=	-I${.CURDIR}/../launchd/freebsd-shims

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

# -lSystemConfiguration: SCDynamicStoreCreate/SetValue — the configd
# publish. -lCoreFoundation: the CFString/CFDictionary/CFBoolean the
# Link dict is built from. -llaunch: bootstrap_port. -lsystem_kernel:
# the Mach port syscalls behind the SC RPCs. -ldispatch
# -lBlocksRuntime: pulled transitively by the SC umbrella header.
LDADD+=		-lSystemConfiguration -lCoreFoundation
LDADD+=		-llaunch -lsystem_kernel
LDADD+=		-ldispatch -lBlocksRuntime -lpthread

BINDIR=		/usr/sbin

.include <bsd.prog.mk>
