# syslogd — Apple's syslog daemon. Phase J2 iter 15 of the ASL port.
# Mach-only ingest first (klog_in, bsd_out, udp_in, remote modules
# can be disabled at init_modules; bsd_in.c is J3). Install to
# /usr/sbin/syslogd. Plan: pkgdemon.github.io/freebsd-asl-plan.html

PROG=		syslogd
MAN=
NO_MAN=		yes
WARNS?=		0

SRCS=		syslogd.c \
		daemon.c \
		dbserver.c \
		asl_action.c \
		bsd_out.c \
		klog_in.c \
		udp_in.c \
		remote.c

MIGOUT?=
.if !empty(MIGOUT)
.PATH:		${MIGOUT}
SRCS+=		asl_ipcServer.c
CFLAGS+=	-I${MIGOUT}
.endif

# Pull aslcommon sources directly (static dep)
.PATH:		${.CURDIR}/../aslcommon
SRCS+=		asl_common.c asl_memory.c

# Headers
CFLAGS+=	-I${.CURDIR}
CFLAGS+=	-I${.CURDIR}/../aslcommon
CFLAGS+=	-I${.CURDIR}/../libsystem_asl.tproj/include
CFLAGS+=	-I${.CURDIR}/../freebsd-shims
CFLAGS+=	-I${.CURDIR}/../../launchd/liblaunch

# Apple build-time defines
CFLAGS+=	-D__APPLE_API_PRIVATE -DPRIVATE=1
CFLAGS+=	-fblocks
CFLAGS+=	-Wno-error=int-conversion -Wno-int-conversion

# Force-include compat shim (LOG_INSTALL etc.)
CFLAGS+=	-include ${.CURDIR}/../freebsd-shims/libasl_freebsd_compat.h

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

LDADD+=		-lsystem_asl -lnotify -lxpc -ldispatch -lsystem_kernel \
		-lpthread -lBlocksRuntime -lbsm -lz

# Re-use notifyd's stub file — it covers most Apple-private link symbols
# both daemons share (audit_token_to_*, sandbox_check_by_audit_token,
# mig_get_special_reply_port, mach_port_construct/destruct, etc.)
.PATH:		${.CURDIR}/../../Libnotify/freebsd-shims
SRCS+=		notifyd_stubs.c

BINDIR=		/usr/sbin

.include <bsd.prog.mk>
