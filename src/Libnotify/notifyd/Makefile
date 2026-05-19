# notifyd — Apple's notification server daemon, ported to FreeBSD.
# Phase J2 iter 1 of the ASL+libnotify port. Runs under PID-1 launchd
# as com.apple.notifyd Mach service. Provides pub/sub notification
# fanout for ASL, configd, IPConfiguration, PowerManagement, etc.
#
# Sources (~5.1k LoC):
#   notifyd.c        daemon main + launchd plist integration
#   notify_proc.c    MIG dispatch handlers (server-side)
#   pathwatch.c      kqueue VNODE filewatchers
#   service.c        service-name registry
# Plus MIG-generated notify_ipcServer.c (passed via MIGOUT).
#
# Install: /usr/sbin/notifyd

PROG=		notifyd
MAN=
NO_MAN=		yes
WARNS?=		0

SRCS=		notifyd.c \
		notify_proc.c \
		pathwatch.c \
		service.c \
		../freebsd-shims/notifyd_stubs.c

MIGOUT?=
.if !empty(MIGOUT)
.PATH:		${MIGOUT}
SRCS+=		notify_ipcServer.c notify_ipcUser.c \
		notify_old_ipcServer.c notify_old_ipcUser.c
CFLAGS+=	-I${MIGOUT}
.endif

# Headers (libnotify + shims + liblaunch's bootstrap_priv.h)
CFLAGS+=	-I${.CURDIR}
CFLAGS+=	-I${.CURDIR}/..
CFLAGS+=	-I${.CURDIR}/../freebsd-shims
CFLAGS+=	-I${.CURDIR}/../../launchd/liblaunch

# Apple build-time defines
CFLAGS+=	-D__APPLE_API_PRIVATE -DPRIVATE=1
CFLAGS+=	-fblocks

# notifyd-specific compat shim (do_notify_subsystem alias, RB_PANIC,
# etc.) — superset of the libnotify_freebsd_compat.h one.
CFLAGS+=	-include ${.CURDIR}/../freebsd-shims/notifyd_freebsd_compat.h

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

# Task #39 Path A: -llaunch provides bootstrap_port + bootstrap_check_in
# (Apple MIG version). notifyd calls these directly at notifyd.c:894.
# Previously satisfied by libxpc's vendored BST copy; libxpc no longer
# ships those symbols.
LDADD+=		-lnotify -lsystem_asl -llaunch -lxpc -ldispatch -lsystem_kernel \
		-lpthread -lBlocksRuntime -lbsm

BINDIR=		/usr/sbin

.include <bsd.prog.mk>
