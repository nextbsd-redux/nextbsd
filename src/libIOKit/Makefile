# libIOKit — the IOKit userland facade (freebsd-launchd-mach K1+K2 of
# the IOKit-userland port plan), iter 1.
#
# A thin CF-flavored client over hwregd's MIG Mach RPC (hwreg.defs).
# Not a port of Apple's IOKitUser source — that library decomposes to
# a kernel device.defs surface this repo has no kernel for; instead
# the facade re-aims the Apple-shape API at hwregd. iter 1 ships the
# read-only registry walk (IORegistryGetRootEntry,
# IORegistryEntryGetChildIterator, IOIteratorNext, GetName, GetPath,
# IOObject{Retain,Release}); later iters add properties + matching,
# the ioreg tool, and notifications.
#
# Build:   cd src/libIOKit && make SYSROOT=... MIGOUT=...
# Install: make DESTDIR=/path/to/rootfs install
#   -> /usr/lib/system/libIOKit.so(.1)
#   -> /usr/include/IOKit/IOKitLib.h

LIB=		IOKit
SHLIB_MAJOR=	1

SRCS=		IOKitLib.c IOKitMatching.c

# hwregUser.c — MIG-generated hwreg.defs client stubs. build.sh runs
# mig (step 3r) and passes MIGOUT=$HWREG_MIG. Same wiring
# libSystemConfiguration's Makefile uses for configUser.c. Pulled in
# only when MIGOUT is set; a bare standalone build (no MIGOUT) fails
# at the generated #include "hwreg.h", which is expected.
MIGOUT?=
.if !empty(MIGOUT)
.PATH:			${MIGOUT}
.PATH:			${.CURDIR}/../hwregd
SRCS+=			hwregUser.c
CFLAGS+=		-I${MIGOUT}
CFLAGS+=		-I${.CURDIR}/../hwregd	# hwreg_mig_types.h
CFLAGS.hwregUser.c+=	-w
# <mach/notify.h> (pulled by the MIG stubs) redefines the MACH_NOTIFY_*
# macros <mach/message.h> also defines — identical values, whitespace
# differs. A benign clash in the vendored mach headers; the hwregd and
# libSystemConfiguration build steps suppress it the same way.
CFLAGS+=		-Wno-macro-redefined
.endif

# WARNS=0: the public header pulls the whole CoreFoundation public
# header tree (for later iters' CFDictionary / CFString surfaces).
# At WARNS>0 the FreeBSD warning machinery adds -Wsystem-headers
# -Werror, which promotes warnings *inside* the vendored CF headers
# (-Wexpansion-to-defined; CFLocking.h's "CF is not thread-safe"
# #warning) into hard errors. libxpc / libCoreFoundation /
# libSystemConfiguration build at WARNS=0 for the same reason.
WARNS=		0
NO_MAN=		yes

# -fblocks: the public header pulls CoreFoundation, and CoreFoundation's
# ForSwiftFoundationOnly.h / CFCalendarPriv.h / CFStreamPriv.h declare
# block-typed function pointers (`void (^block)(...)`). Without -fblocks
# clang errors out with "blocks support disabled" on every include
# site. libSystemConfiguration / libxpc carry -fblocks for the same
# reason.
CFLAGS+=	-O2 -pipe -fblocks

# -I${.CURDIR} MUST come first so <IOKit/IOKitLib.h> resolves to this
# library's public header (src/libIOKit/IOKit/IOKitLib.h) rather than
# the small launchctl stub at src/launchd/freebsd-shims/IOKit/
# IOKitLib.h that the freebsd-shims -I below also covers. Search
# order matches first; that umbrella -I also picks up IOKitInternal.h.
CFLAGS+=	-I${.CURDIR}

# <servers/bootstrap.h> (bootstrap_look_up + the bootstrap_port
# global) comes from liblaunch; its bootstrap.h pulls Apple shim
# headers from launchd/freebsd-shims. -I libxpc gives IOKitMatching.c
# the `nv.h` nvlist API (hwregd's property bag + hwreg_lookup
# criteria cross MIG as packed nvlist blobs).
CFLAGS+=	-I${.CURDIR}/../launchd/liblaunch
CFLAGS+=	-I${.CURDIR}/../launchd/freebsd-shims
CFLAGS+=	-I${.CURDIR}/../libxpc

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDADD+=		-L${SYSROOT}/usr/lib/system
.endif

INCS=		IOKit/IOKitLib.h
INCSDIR=	${PREFIX}/include/IOKit
LIBDIR=		${PREFIX}/lib/system

# -lCoreFoundation: the CFDictionary / CFString / CFNumber object
# model behind the iter-2 property and matching surface (the public
# header pulls CoreFoundation too). -lxpc: the nvlist API
# (nvlist_unpack / pack / next / get_*) IOKitMatching.c uses to
# bridge hwregd's property bag and lookup criteria. -lsystem_kernel:
# mach_msg + the Mach port syscalls behind the MIG client stubs.
# -llaunch: bootstrap_look_up + bootstrap_port. -lpthread:
# pthread_once for the lazy hwregd service-port lookup.
LDADD+=		-lCoreFoundation -lxpc
LDADD+=		-lsystem_kernel -llaunch -lpthread
LDADD+=		-Wl,--allow-shlib-undefined
LDADD+=		-Wl,-rpath,${PREFIX}/lib/system

PREFIX?=	/usr

.include <bsd.lib.mk>
