# aslmanager — Apple's ASL log rotator. Phase J4 of the ASL port.
# Invoked on demand by launchd (com.apple.aslmanager.plist) to age
# out /var/log/asl/* into Archive/. Installed at /usr/sbin/.

PROG=		aslmanager
MAN=
NO_MAN=		yes
WARNS?=		0

SRCS=		aslmanager.c daemon.c

# Pull aslcommon sources (static dep — same pattern as syslogd)
.PATH:		${.CURDIR}/../aslcommon
SRCS+=		asl_common.c asl_memory.c

# Re-use notifyd's stub file — covers audit_token, sandbox,
# mig_*, voucher_*, vm_alloc/dealloc, _malloc_no_asl_log, copyfile
.PATH:		${.CURDIR}/../../Libnotify/freebsd-shims
SRCS+=		notifyd_stubs.c

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

# Force-include compat shim (LOG_INSTALL, OSSpinLock, IOPOL_*, etc.)
CFLAGS+=	-include ${.CURDIR}/../freebsd-shims/libasl_freebsd_compat.h

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

LDADD+=		-lsystem_asl -lnotify -llaunch -lxpc -ldispatch \
		-lsystem_kernel -lpthread -lBlocksRuntime -lbsm -lz

BINDIR=		/usr/sbin

.include <bsd.prog.mk>
