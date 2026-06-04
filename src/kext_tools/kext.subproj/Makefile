# libkext — Apple's OSKext engine (faithful #182 port), kld-backed.
#
# Static library linked by the kext_tools CLIs (kextdeps, and eventually the
# OSKext-backed kextload/kextstat/kextunload). Built against a NextBSD sysroot
# (SYSROOT=...) for the libCoreFoundation / libIOKit headers.
#
# The XNU load/unload/query paths are re-backed onto kld; kxld/mkext/prelink/
# kext_request are removed; personalities are kextd's job (#177). macho_util.c /
# fat_util.c are Apple's Mach-O inspectors — kept verbatim and backed by the
# compat swap/arch shim (macho_compat.c); they do not apply to ELF .ko kexts
# and are not exercised by dependency resolution (Phase 1).
#
# Build:  cd src/kext_tools/kext.subproj && make SYSROOT=/path/to/rootfs

LIB=		kext
INTERNALLIB=	yes		# build libkext.a only; CLIs link it by path

KT=		${.CURDIR}/..

SRCS=		OSKext.c OSKextVersion.c misc_util.c printPList_new.c \
		macho_util.c fat_util.c

.PATH:		${KT}/compat
SRCS+=		macho_compat.c iocf_compat.c

# Apple's macho_util.c / fat_util.c use Mach's boolean_t + TRUE/FALSE
# (<mach/boolean.h>), absent on FreeBSD. Force-include the shim into ONLY these
# two vendored files so it can't clash with OSKext.c's own boolean_t.
CFLAGS.macho_util.c+=	-include ${KT}/compat/nextbsd_mach_types.h
CFLAGS.fat_util.c+=	-include ${KT}/compat/nextbsd_mach_types.h

# FreeBSD 15 links executables as PIE; static-lib objects pulled into a PIE must
# be position-independent or ld rejects R_X86_64_32 relocations against locals.
CFLAGS+=	-fPIC

CFLAGS+=	-I${KT}/compat -I${.CURDIR} -I${KT}/../libIOKit
CFLAGS+=	-include ${KT}/compat/apple-availability-shim.h
CFLAGS+=	-fblocks -fconstant-cfstrings
CFLAGS+=	-DPRIVATE=1 -D__APPLE_API_PRIVATE -DTARGET_OS_OSX=1 -DTARGET_OS_MAC=1
.if !empty(SYSROOT)
CFLAGS+=	-isystem ${SYSROOT}/usr/include
.endif

# Vendored Apple sources: silence the FreeBSD warning machinery (same posture
# as libIOKit / libxpc / libCoreFoundation).
WARNS=		0
CFLAGS+=	-Wno-everything
NO_MAN=		yes

.include <bsd.lib.mk>
