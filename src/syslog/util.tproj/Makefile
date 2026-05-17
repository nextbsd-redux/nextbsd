# syslog(1) — Apple's CLI for ASL. Phase J5 of the ASL port.
# Reads/writes ASL log database, can post messages and pretty-print
# the on-disk asl store. Installed at /usr/bin/syslog.
# Conflicts with FreeBSD base's logger(1) but doesn't share a name,
# and base has no /usr/bin/syslog.

PROG=		syslog
MAN=
NO_MAN=		yes
WARNS?=		0

SRCS=		syslog.c

# Pull aslcommon sources (static dep — same pattern as syslogd/aslmanager)
.PATH:		${.CURDIR}/../aslcommon
SRCS+=		asl_common.c

# Re-use notifyd's link-stubs file
.PATH:		${.CURDIR}/../../Libnotify/freebsd-shims
SRCS+=		notifyd_stubs.c

# MIG-generated asl_ipcUser.c for direct talk to syslogd
MIGOUT?=
.if !empty(MIGOUT)
.PATH:		${MIGOUT}
SRCS+=		asl_ipcUser.c
CFLAGS+=	-I${MIGOUT}
.endif

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

# Force-include compat shim
CFLAGS+=	-include ${.CURDIR}/../freebsd-shims/libasl_freebsd_compat.h

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

LDADD+=		-lsystem_asl -lnotify -llaunch -lxpc -ldispatch \
		-lsystem_kernel -lpthread -lBlocksRuntime -lbsm

BINDIR=		/usr/bin

.include <bsd.prog.mk>
