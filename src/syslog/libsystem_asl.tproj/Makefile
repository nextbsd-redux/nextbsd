# libsystem_asl — Apple's ASL (Apple System Log) client library.
# Vendored from apple-oss-distributions/syslog @ 69d8586.
# Phase J1 iter 1: client library only. syslogd / aslmanager / syslog(1)
# are J2+.
#
# 14 source files in src/, ~14.6k LoC. Plus aslcommon library
# (separate dir, linked statically). Plus MIG-generated asl_ipcUser.c
# (passed via MIGOUT, also rolled into aslcommon).
#
# Install: /usr/lib/system/libsystem_asl.so + sonname symlink.
# Public headers: asl.h, asl_msg.h, asl_msg_list.h, asl_object.h
# → /usr/include/. Private: asl_private.h etc → /usr/include/asl/.

LIB=		system_asl
SHLIB_MAJOR=	1

WARNS?=		0
NO_MAN=		yes

SRCS=		asl.c \
		asl_msg.c \
		asl_file.c \
		asl_core.c \
		asl_store.c \
		asl_legacy1.c \
		asl_string.c \
		asl_client.c \
		asl_msg_list.c \
		asl_object.c \
		asl_util.c \
		asl_fd.c \
		syslog.c \
		asl_mt_shim.c \
		asl_stubs.c

# Pull aslcommon sources into our build (so .so includes them) —
# matches Apple Xcode's pattern where libsystem_asl re-uses the
# aslcommon objects.
.PATH:		${.CURDIR}/src
.PATH:		${.CURDIR}/../aslcommon
SRCS+=		asl_common.c asl_memory.c

# MIG-generated client stubs from asl_ipc.defs
MIGOUT?=
.if !empty(MIGOUT)
.PATH:		${MIGOUT}
SRCS+=		asl_ipcUser.c
CFLAGS+=	-I${MIGOUT}
.endif

# Headers
CFLAGS+=	-I${.CURDIR}/include
CFLAGS+=	-I${.CURDIR}/../aslcommon
CFLAGS+=	-I${.CURDIR}/../freebsd-shims
CFLAGS+=	-I${.CURDIR}/../../launchd/liblaunch		# bootstrap_priv.h

# Apple build-time defines
CFLAGS+=	-D__APPLE_API_PRIVATE -DPRIVATE=1
CFLAGS+=	-fblocks

# Vendored Apple source has a couple of int->pointer return bugs
# in asl_msg_list_match (returns n instead of out, lines 448/453);
# Apple's build presumably tolerated this. Downgrade to warning.
CFLAGS+=	-Wno-error=int-conversion -Wno-int-conversion

# Force-include compat shim — LOG_INSTALL etc syslog facilities Apple
# defines but FreeBSD doesn't ship.
CFLAGS+=	-include ${.CURDIR}/../freebsd-shims/libasl_freebsd_compat.h

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
# -Wl,-z,defs: fail at link time if any symbol is unresolved (default
# for .so is to allow). Surfaces all dlopen-time symbol misses in
# one build so we can stub them in asl_stubs.c instead of whack-a-mole
# at runtime. Phase J runtime iter 6.
LDFLAGS+=	-Wl,--no-allow-shlib-undefined -Wl,-z,defs
.endif

LDADD+=		-lxpc -ldispatch -lsystem_kernel -lpthread -lBlocksRuntime
LDADD+=		-lbsm		# audit_token_to_au32 from FreeBSD base
LDADD+=		-lnotify	# notify_post / notify_register_* (asl_action.c)
LDADD+=		-llaunch	# bootstrap_look_up2 / bootstrap_priv

# Install path
LIBDIR=		/usr/lib/system
SHLIBDIR=	/usr/lib/system

# Public headers
INCS=		include/asl.h \
		include/asl_msg.h \
		include/asl_msg_list.h \
		include/asl_object.h
INCSDIR=	/usr/include

.include <bsd.lib.mk>
