PROG=	kextunload
SRCS=	kextunload.c

# Graduated to the OSKext engine (#196): links libkext.a (../kext.subproj) +
# CoreFoundation + IOKit + zlib, same as kextdeps. Built AFTER libIOKit by
# build.sh (kextdeps needs IORegistryEntryCreateCFProperty). OSKext.h needs the
# compat headers + availability shim; WARNS=0/-Wno-everything matches the lib.
KT=	${.CURDIR}/..

CFLAGS+=	-I${KT}/kext.subproj -I${KT}/compat -I${KT}/../libIOKit
CFLAGS+=	-include ${KT}/compat/apple-availability-shim.h
CFLAGS+=	-DPRIVATE=1 -D__APPLE_API_PRIVATE
WARNS=		0
CFLAGS+=	-Wno-everything

LDADD+=	${KT}/kext.subproj/libkext.a -lCoreFoundation -lIOKit -lz
DPADD+=	${KT}/kext.subproj/libkext.a

.include <bsd.prog.mk>
