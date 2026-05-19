# libxpc — Apple-canonical XPC userland library, ported from ravynOS's
# lib/libxpc/. Uses our libsystem_kernel for Mach trap access and our
# vendored libdispatch for queue/source support. The bootstrap_check_in
# / bootstrap_look_up dependencies are satisfied by libbootstrap (in
# src/bootstrap/), which is built into both the daemon and the smoke
# tests for now and will become a real /usr/lib/system/libbootstrap.so
# once it stabilizes.
#
# Naming: matches Apple's libxpc.dylib on macOS so Apple-source code that
# links -lxpc compiles unchanged. Install path follows the Phase D
# layout — /usr/lib/system/libxpc.so. Headers at /usr/include/xpc/.
#
# Vendored from ravynOS, which itself imported from NextBSD. Local subr_
# nvlist.c + subr_nvpair.c carry their own copies of FreeBSD libnv (the
# only consumer of libnv in this stack), so we don't need to link -lnv;
# the symbols are statically rolled in.
#
# Build:   cd src/libxpc && make
# Install: make DESTDIR=/path/to/rootfs install

LIB=		xpc
SHLIB_MAJOR=	4

# Path A (task #39, 2026-05-19): bootstrap_check_in / _look_up /
# bootstrap_port now come from liblaunch.so (the Apple MIG-based
# version). Previously libxpc vendored src/bootstrap/libbootstrap.c
# (our hand-rolled BST protocol), which baked BST `bootstrap_check_in`
# into libxpc.so. Any xpc_connection_* path that called
# bootstrap_check_in internally always resolved to that BST copy
# regardless of link order — and launchd's MIG demux doesn't speak
# BST, so a single launchctl call panicked init. Drop the vendoring;
# link -llaunch dynamically.
#
# src/bootstrap/libbootstrap.c is still consumed (compiled directly)
# by Phase G2 tests + bootstrap_server which DON'T link libxpc, so
# no symbol collision and they keep their BST-on-the-wire test
# infrastructure.

SRCS=		xpc_dictionary.c \
		xpc_misc.c \
		xpc_type.c \
		xpc_array.c \
		xpc_connection.c \
		subr_nvlist.c \
		subr_nvpair.c \
		msgio.c \
		stubs.c

# SYSROOT — points at the in-build rootfs so we can find the headers
# libxpc consumes (mach/*, dispatch/*, servers/bootstrap.h) and the
# libs it links against (libsystem_kernel, libdispatch). build.sh
# passes SYSROOT=$WORK/rootfs; unset on standalone host build.
SYSROOT?=

CFLAGS+=	-I${.CURDIR} -I${.CURDIR}/../bootstrap
CFLAGS+=	-fblocks
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}${PREFIX}/include
LDADD+=		-L${SYSROOT}${PREFIX}/lib/system
.endif
NO_MAN=		yes
WARNS?=		0	# vendored code; don't gate on style warnings

INCS=		xpc/activity.h \
		xpc/base.h \
		xpc/connection.h \
		xpc/debug.h \
		xpc/endpoint.h \
		xpc/launchd.h \
		xpc/private.h \
		xpc/xpc.h
INCSDIR=	${PREFIX}/include/xpc
LIBDIR=		${PREFIX}/lib/system

# Link against libsystem_kernel for Mach trap shims and libdispatch
# for queue/source primitives. Both are at /usr/lib/system/ per
# the Phase D layout. -lBlocksRuntime resolves _Block_copy /
# _NSConcreteStackBlock etc. from -fblocks; -lsbuf resolves the
# sbuf_* API xpc_misc.c uses for xpc_copy_description. -rpath lets
# rtld find our libsystem dir at runtime.
LDADD+=		-lsystem_kernel -ldispatch -lBlocksRuntime -lsbuf -lpthread
# bootstrap_check_in / bootstrap_look_up / bootstrap_port are now
# provided by liblaunch.so (Apple MIG protocol — see SRCS comment
# above for the Path A migration). Cannot link -llaunch here without
# reordering build.sh because liblaunch builds AFTER libxpc; use
# --allow-shlib-undefined so libxpc.so accepts these symbols as
# unresolved at build time. At runtime, every consumer that loads
# libxpc.so also links liblaunch.so (the daemons via -llaunch -lxpc;
# launchctl via -llaunch -lxpc), so the rtld dependency graph
# provides them.
LDADD+=		-Wl,--allow-shlib-undefined
LDADD+=		-Wl,-rpath,${PREFIX}/lib/system

PREFIX?=	/usr

.include <bsd.lib.mk>
