PROG=	kextd
SRCS=	kextd.c
MAN=

# kextd pushes kext IOKitPersonalities into the in-kernel IOCatalogue (U1,
# nextbsd#217). Same link set as kextload/kextdeps: libkext.a (Apple's OSKext
# engine, #182) + CoreFoundation + IOKit + zlib; built by build.sh AFTER
# libIOKit. The IOCatalogue ABI header (sys/iocatalogue.h) is vendored here
# (canonical copy lives in nextbsd-kernel's src-overlay) and included locally,
# so the build doesn't depend on it being installed into the sysroot first.
#
# Installs to /usr/libexec/kextd (Apple's location). `-w` (watch) runs it as the
# persistent daemon: register HOST_KEXTD_PORT, push personalities, serve kernel
# load requests (K3b, #216). -lsystem_kernel provides mach_msg / mach_port_* /
# host_set_special_port for the receive loop + port registration.
BINDIR=	/usr/libexec

KT=	${.CURDIR}/..

CFLAGS+=	-I${.CURDIR}
CFLAGS+=	-I${KT}/kext.subproj -I${KT}/compat -I${KT}/../libIOKit
CFLAGS+=	-include ${KT}/compat/apple-availability-shim.h
CFLAGS+=	-DPRIVATE=1 -D__APPLE_API_PRIVATE
WARNS=		0
CFLAGS+=	-Wno-everything

LDADD+=	${KT}/kext.subproj/libkext.a -lCoreFoundation -lIOKit -lz -lsystem_kernel
DPADD+=	${KT}/kext.subproj/libkext.a

.include <bsd.prog.mk>
