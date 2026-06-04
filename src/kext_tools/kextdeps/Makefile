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
CFLAGS+=	-DPRIVATE=1 -D__APPLE_API_PRIVATE -DTARGET_OS_OSX=1 -DTARGET_OS_MAC=1

LDADD+=	${KT}/kext.subproj/libkext.a -lCoreFoundation -lIOKit
DPADD+=	${KT}/kext.subproj/libkext.a

.include <bsd.prog.mk>
