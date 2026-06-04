PROG=	kextdeps
SRCS=	kextdeps.c

# Links libkext.a (Apple's OSKext engine, ../kext.subproj) + CoreFoundation +
# IOKit from the sysroot. The shared CF flags + -L${SYSROOT}/usr/lib/system +
# rpath + --allow-shlib-undefined come from ../Makefile.inc (same as the trio).
# OSKext.h needs the compat headers + the availability shim, exactly as the
# compile-check harness does.

KT=	${.CURDIR}/..

CFLAGS+=	-I${KT}/kext.subproj -I${KT}/compat -I${KT}/../libIOKit
CFLAGS+=	-include ${KT}/compat/apple-availability-shim.h
CFLAGS+=	-DPRIVATE=1 -D__APPLE_API_PRIVATE

# Vendored Apple headers (OSKext.h + CoreFoundation) redefine TARGET_OS_* /
# SYNC_POLICY_* macros; at WARNS>0 the FreeBSD warning machinery -Werror's
# -Wmacro-redefined. Match the libkext build posture (WARNS=0 + -Wno-everything),
# same as libIOKit / libxpc / libCoreFoundation consumers.
WARNS=		0
CFLAGS+=	-Wno-everything

# libkext + CoreFoundation + zlib only. No -lIOKit: libkext's iocf_compat.c
# provides the IOCFSerialize/IOCFUnserialize symbols OSKext needs, so nothing
# is pulled from libIOKit — and it isn't installed in the rootfs at this build
# step anyway. (-lz: OSKext's cache paths use inflate*/gz*.)
LDADD+=	${KT}/kext.subproj/libkext.a -lCoreFoundation -lz
DPADD+=	${KT}/kext.subproj/libkext.a

.include <bsd.prog.mk>
