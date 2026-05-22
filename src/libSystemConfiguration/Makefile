# libSystemConfiguration — the SystemConfiguration client framework
# (freebsd-launchd-mach port of Apple's SystemConfiguration.fproj).
#
# This library is what a client links against (-lSystemConfiguration)
# to talk to configd: it wraps configd's MIG `config` Mach subsystem
# (config.defs) in the CoreFoundation-typed SCDynamicStore* API. iter 1
# is the synchronous store API — SCDynamicStoreCreate plus get / set /
# add / remove / list. Change notifications are a later iteration.
#
# Like configd itself, this depends on the MIG-generated config.defs
# client stubs (configUser.c) and so cannot build without MIGOUT —
# build.sh runs mig and passes MIGOUT=; a bare standalone build fails
# at the generated #include "config.h", which is expected.
#
# Build:   cd src/libSystemConfiguration && make SYSROOT=... MIGOUT=...
# Install: make DESTDIR=/path/to/rootfs install
#   -> /usr/lib/system/libSystemConfiguration.so(.1)
#   -> /usr/include/SystemConfiguration/*.h

LIB=		SystemConfiguration
SHLIB_MAJOR=	1

SRCS=		SCD.c SCDynamicStore.c SCNotify.c SCDMultiple.c

# configUser.c — MIG-generated config.defs client stubs (same file
# configd's test clients link). config_wire.c — configd's shared
# length-prefixed key-list decoder, reused by SCDynamicStoreCopyKeyList
# to unpack the configlist reply. Both are pulled in only when MIGOUT
# is set; see the header comment.
MIGOUT?=
.if !empty(MIGOUT)
.PATH:			${MIGOUT}
.PATH:			${.CURDIR}/../configd
SRCS+=			configUser.c config_wire.c
CFLAGS+=		-I${MIGOUT}
CFLAGS+=		-I${.CURDIR}/../configd	# config_types.h, config_wire.h
CFLAGS.configUser.c+=	-w
# <mach/notify.h> (pulled by the MIG stubs) redefines the MACH_NOTIFY_*
# macros <mach/message.h> also defines — identical values, whitespace
# differs. A benign clash in the vendored mach headers; configd's
# Makefile suppresses it the same way.
CFLAGS+=		-Wno-macro-redefined
.endif

# WARNS=0: this library pulls the whole CoreFoundation public header
# tree. At WARNS>0 the FreeBSD warning machinery adds -Wsystem-headers
# -Werror, which promotes warnings *inside* the vendored CF headers
# (-Wexpansion-to-defined in CFBundle.h/CFPriv.h/CFRuntime.h, and
# CFLocking.h's "CF is not thread-safe" #warning) into hard build
# errors. libxpc and libCoreFoundation — the repo's other CF-tree
# components — build at WARNS=0 for the same reason.
WARNS=		0
NO_MAN=		yes

CFLAGS+=	-O2 -pipe -fblocks
CFLAGS+=	-I${.CURDIR}		# <SystemConfiguration/...> umbrella path

# <servers/bootstrap.h> (bootstrap_look_up + the bootstrap_port global)
# comes from liblaunch; its bootstrap.h pulls Apple shim headers from
# launchd/freebsd-shims.
CFLAGS+=	-I${.CURDIR}/../launchd/liblaunch
CFLAGS+=	-I${.CURDIR}/../launchd/freebsd-shims

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDADD+=		-L${SYSROOT}/usr/lib/system
.endif

INCS=		SystemConfiguration/SCDynamicStore.h \
		SystemConfiguration/SystemConfiguration.h
INCSDIR=	${PREFIX}/include/SystemConfiguration
LIBDIR=		${PREFIX}/lib/system

# -lCoreFoundation: the CF object model + property-list serialization.
# -lsystem_kernel: mach_msg and the Mach port syscalls behind the MIG
# client stubs. -llaunch: bootstrap_look_up + bootstrap_port.
# -ldispatch: the DISPATCH_SOURCE_TYPE_MACH_RECV source behind
# SCDynamicStoreSetDispatchQueue. -lBlocksRuntime: the dispatch source
# handlers are blocks (-fblocks).
LDADD+=		-lCoreFoundation -ldispatch -lBlocksRuntime
LDADD+=		-lsystem_kernel -llaunch -lpthread
LDADD+=		-Wl,--allow-shlib-undefined
LDADD+=		-Wl,-rpath,${PREFIX}/lib/system

PREFIX?=	/usr

.include <bsd.lib.mk>
