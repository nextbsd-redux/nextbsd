# libnotify — Apple's notification client library, ported to FreeBSD.
#
# Vendored from apple-oss-distributions/Libnotify @ 715d461778f6b93c821d99390a0078bd6f6d8c04.
# Phase J1 iter 1: client library only (libnotify.so). The notifyd
# daemon is J2, ships separately.
#
# Sources (per Apple's Xcode project, client-only subset):
#   libnotify.c       — primary client implementation (~1228 LoC)
#   notify_client.c   — MIG-using client API (~4752 LoC)
#   table.c           — hash table support (small)
#
# MIG-generated client stubs (passed via MIGOUT):
#   notify_ipcUser.c       — from notify_ipc.defs
#   notify_old_ipcUser.c   — from notify_old_ipc.defs
#
# Apple-only header surface satisfied by ./freebsd-shims/os/* +
# ./freebsd-shims/notify_probes.h (see headers' comments).
#
# Install: /usr/lib/system/libnotify.so + sonname symlink, headers
# notify.h + notify_keys.h to /usr/include/.

LIB=		notify
SHLIB_MAJOR=	1

# Vendored Apple source — don't gate on its warnings.
WARNS?=		0
NO_MAN=		yes

SRCS=		libnotify.c \
		notify_client.c \
		table.c

# MIGOUT — directory holding the mig-generated *User.c / *.h.
# build.sh sets this to $WORK/libnotify-mig. Empty on standalone
# build; build will fail at the first generated #include, expected.
MIGOUT?=

# SYSROOT — in-build rootfs. Headers libnotify needs that aren't
# FreeBSD-native: <dispatch/*>, <xpc/*>, <mach/*>, <servers/bootstrap.h>.
SYSROOT?=

.if !empty(MIGOUT)
.PATH:		${MIGOUT}
SRCS+=		notify_ipcUser.c notify_old_ipcUser.c
CFLAGS+=	-I${MIGOUT}
.endif

# Local headers + Apple-shim headers
CFLAGS+=	-I${.CURDIR}
CFLAGS+=	-I${.CURDIR}/freebsd-shims

# liblaunch ships bootstrap_priv.h (notify_client.c uses
# bootstrap_look_up2 + BOOTSTRAP_PRIVILEGED_SERVER from it). Pull
# from the in-tree liblaunch source rather than installing into
# SYSROOT — header is small and the dependency is one-way.
CFLAGS+=	-I${.CURDIR}/../launchd/liblaunch

# Apple build-time defines from xcodeconfig/.
CFLAGS+=	-D__APPLE_API_PRIVATE -DPRIVATE=1
CFLAGS+=	-fblocks		# libdispatch block syntax

# Force-include compat shim so notify_client.c sees ASL_LEVEL_*
# constants without needing #include <asl.h> in Apple's source.
# Same pattern as launchd's launchctl_freebsd_compat.h.
CFLAGS+=	-include ${.CURDIR}/freebsd-shims/libnotify_freebsd_compat.h

.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
# libxpc/libdispatch may have hidden-visibility refs to symbols
# we haven't fully wired yet — same allowance launchctl needs.
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

LDADD+=		-lxpc -ldispatch -lsystem_kernel -lpthread -lBlocksRuntime

# Install path
LIBDIR=		/usr/lib/system
SHLIBDIR=	/usr/lib/system

# Public headers
INCS=		notify.h notify_keys.h
INCSDIR=	/usr/include

.include <bsd.lib.mk>
