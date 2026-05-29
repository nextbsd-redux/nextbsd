# launchd — Apple's launchd daemon, ported to FreeBSD.
#
# Vendored from apple-oss-distributions/launchd @ launchd-842.92.1.
# Seven hand-written translation units (launchd.c core.c runtime.c
# ipc.c log.c kill2.c ktrace.c) plus the MIG-generated RPC stubs for
# the job / job_forward / job_reply / internal / helper / mach_exc /
# notify subsystems.
#
# Install path: /sbin/launchd, matching Apple and the project's
# install-layout spike (freebsd-libxpc-install-layout-spike.html
# section 7). PID-1 promotion is a separate, later step — the binary
# at /sbin/launchd is the same whether it's started by init or runs
# as init.
#
# Build:   cd src/launchd/src && make
# Install: make DESTDIR=/path/to/rootfs install
#
# Depends at build time on:
#   - the MIG stubs generated in $WORK/launchd-mig — build.sh passes
#     MIGOUT=...; .PATH below pulls the *User.c / *Server.c into SRCS
#   - liblaunch (built by the prior step, installed in SYSROOT)
#   - our libmach / libxpc / libdispatch headers + libs (SYSROOT)
#   - the FreeBSD shim headers in ../freebsd-shims
#   - launchd's own liblaunch/ public headers (launch.h, vproc.h, ...)

PROG=		launchd
MAN=

# MIGOUT — directory holding the mig-generated *User.c / *Server.c /
# *.h. build.sh sets this to $WORK/launchd-mig. Empty on a bare
# standalone build — the build then fails at the first generated
# #include, which is expected; launchd can't build without MIG output.
MIGOUT?=

# SYSROOT — the in-build rootfs. Earlier build.sh steps install there
# the headers + libs launchd needs but that aren't FreeBSD-native:
# liblaunch.so, libxpc.so, libdispatch.so, libsystem_kernel.so, plus
# <uuid/uuid.h>, <dispatch/*>, <xpc/*>, <servers/bootstrap.h>, the
# <mach/mach.h> umbrella. build.sh passes SYSROOT=$WORK/rootfs.
SYSROOT?=

SRCS=		launchd.c \
		core.c \
		runtime.c \
		ipc.c \
		log.c \
		kill2.c \
		ktrace.c \
		forward_stubs.c \
		launchd_plist_scan.c \
		launchd_early.c

# MIG-generated stubs. launchd is both client and server for the
# job / internal / helper subsystems, server-only for mach_exc and
# notify, client-only for job_reply / job_forward. Apple's Xcode
# project compiles only the side it uses for each .defs (see the
# ATTRIBUTES in xcode-project.pbxproj.reference). We do the same:
# job_replyServer.c is excluded because bootstrap_cmds-138's MIG
# emits a broken server demux for reply-only subsystems (references
# an unpacked `kr` variable that's never declared). The user-side
# job_replyUser.c is what launchd actually uses to send back replies.
.if !empty(MIGOUT)
.PATH:		${MIGOUT}
SRCS+=		jobUser.c jobServer.c \
		job_forwardUser.c job_forwardServer.c \
		job_replyUser.c \
		internalUser.c internalServer.c \
		helperUser.c helperServer.c \
		mach_excUser.c mach_excServer.c \
		notifyUser.c notifyServer.c
.endif

# Vendored Apple source — don't gate on its warnings.
WARNS?=		0
NO_MAN=		yes

CFLAGS+=	-I${.CURDIR}			# config.h, core.h, launchd.h, ...
CFLAGS+=	-I${.CURDIR}/../liblaunch	# launch.h, vproc.h, bootstrap.h, ...
CFLAGS+=	-I${.CURDIR}/../freebsd-shims	# libkern/, os/, libproc.h, ...
CFLAGS+=	-I${.CURDIR}/../../libmach/include	# mach/*
.if !empty(MIGOUT)
CFLAGS+=	-I${MIGOUT}			# job.h, jobServer.h, mach_excServer.h, ...
.endif
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include	# uuid/, dispatch/, xpc/, servers/
LDADD+=		-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
# libCoreFoundation.so has internal hidden-visibility references
# (_CFThreadSetName, _CFGetCurrentDirectory, ...) that FreeBSD's
# bsd.prog.mk strict --no-allow-shlib-undefined catches at link.
# Same situation as launchctl's Makefile (memory: linker policy
# difference, not a launchd bug). Match the policy.
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

# launchd-842 expects these Apple build-time defines.
CFLAGS+=	-D__APPLE_API_PRIVATE -DPRIVATE=1
CFLAGS+=	-fblocks			# libdispatch block syntax
CFLAGS+=	-fconstant-cfstrings		# CFSTR() literal -> CFStringRef (launchd_plist_scan.c)

# CF + ICU consumer flags — required for launchd_plist_scan.c which
# links libCoreFoundation + lib_FoundationICU to parse LaunchDaemons
# plists at PID-1 startup. MUST match libicu's compile env per
# memory icu-complete-corefoundation-ok (otherwise rtld eagerly
# fails at first dlopen on swift_-prefixed ICU symbols):
#   __HAS_APPLE_ICU__=1   activate CF's Apple-private ICU code paths
#   U_DISABLE_RENAMING=1  match libicu's symbol export scheme
#   APPLE_ICU_CHANGES=1   expose Apple-private ICU function decls
CFLAGS+=	-D__HAS_APPLE_ICU__=1
CFLAGS+=	-DU_DISABLE_RENAMING=1
CFLAGS+=	-DAPPLE_ICU_CHANGES=1

LDADD+=		-llaunch -lxpc -ldispatch -lsystem_kernel \
		-lCoreFoundation -l_FoundationICU \
		-lpthread -lutil -lBlocksRuntime \
		-lm		# floor/fmod/scalbn/ceil pulled in by CF

BINDIR=		/sbin

.include <bsd.prog.mk>
