# aslcommon — static lib shared by libsystem_asl + syslogd + util +
# aslmanager. Source: 3 .c files (~4.5k LoC):
#   asl_common.c    — config-file parser, module merge
#   asl_memory.c    — in-memory ASL message store
# Plus the MIG-generated asl_ipcUser.c + asl_ipcServer.c (passed in
# via MIGOUT).
#
# This produces libaslcommon.a — linked into all consumers but
# never installed standalone.

LIB=		aslcommon
INTERNALLIB=	yes
NO_PIC=		# nope, needs PIC since libsystem_asl.so links it

WARNS?=		0
NO_MAN=		yes

SRCS=		asl_common.c \
		asl_memory.c

MIGOUT?=
.if !empty(MIGOUT)
.PATH:		${MIGOUT}
SRCS+=		asl_ipcUser.c asl_ipcServer.c
CFLAGS+=	-I${MIGOUT}
.endif

# Local headers + sibling headers
CFLAGS+=	-I${.CURDIR}
CFLAGS+=	-I${.CURDIR}/../libsystem_asl.tproj/include
CFLAGS+=	-I${.CURDIR}/../freebsd-shims
CFLAGS+=	-I${.CURDIR}/../../launchd/liblaunch		# bootstrap_priv.h

# Apple build-time defines
CFLAGS+=	-D__APPLE_API_PRIVATE -DPRIVATE=1
CFLAGS+=	-fblocks
CFLAGS+=	-fPIC

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
.endif

.include <bsd.lib.mk>
