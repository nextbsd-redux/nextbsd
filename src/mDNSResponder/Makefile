# mDNSResponder — freebsd-launchd-mach Bonjour/zeroconf daemon.
#
# iter 2: vendor mDNSCore + mDNSPosix + mDNSShared + DSO (the Apple
# upstream protocol engine + POSIX platform binding + uds_daemon
# AF_UNIX server + DNS Stateful Operations) and link them into the
# `mDNSResponder` binary. PosixDaemon.c (upstream) is the entry
# point; mach_bridge.c claims the com.apple.mDNSResponder Mach
# service from inside main() so peer bootstrap_look_up succeeds.
# iter 3 grows the dns_sd.defs MIG IDL so Apple-shape clients can
# use either AF_UNIX (legacy libdns_sd) or Mach RPC for DNS-SD
# requests.
#
# Builds /usr/sbin/mDNSResponder; runs as a launchd-managed daemon
# under com.apple.mDNSResponder.plist in overlays/.
#
# Build:   cd src/mDNSResponder && make SYSROOT=/path/to/rootfs
# Install: make DESTDIR=/path/to/rootfs install

PROG=		mDNSResponder
MAN=

# Source layout follows upstream Apple's mDNSPosix/Makefile DAEMONOBJS
# (lines 254-259 there). PosixDaemon.c is the entry point; mach_bridge.c
# is our Mach-service hook. Each vendored .c is referenced by basename
# and resolved via .PATH below.
SRCS=		mach_bridge.c
SRCS+=		PosixDaemon.c mDNSPosix.c mDNSUNP.c
SRCS+=		mDNS.c DNSDigest.c uDNS.c DNSCommon.c
SRCS+=		uds_daemon.c mDNSDebug.c dnssd_ipc.c GenLinkedList.c
SRCS+=		PlatformCommon.c ClientRequests.c dnssd_clientshim.c
SRCS+=		mdns_addr_tailq.c misc_utilities.c
SRCS+=		dso.c dso-transport.c

.PATH:		${.CURDIR}/mDNSCore
.PATH:		${.CURDIR}/mDNSPosix
.PATH:		${.CURDIR}/mDNSShared
.PATH:		${.CURDIR}/DSO

# Vendored Apple code is not WARNS=6-clean (lots of -Wunused-but-set-
# variable, -Wparentheses, etc.). Same trade configd / libxpc / libCF /
# libSystemConfiguration take.
WARNS=		0
NO_MAN=		yes

# Upstream's CFLAGS_COMMON + the FreeBSD ifeq block at mDNSPosix/Makefile
# lines 65 + 136-150.
CFLAGS+=	-O2 -pipe -fwrapv
CFLAGS+=	-DPOSIX_BUILD
CFLAGS+=	-DPID_FILE=\"/var/run/mDNSResponder.pid\"
CFLAGS+=	-DMDNS_UDS_SERVERPATH=\"/var/run/mDNSResponder\"
CFLAGS+=	-DMDNS_NO_STRICT=1
CFLAGS+=	-DMDNS_DEBUGMSGS=0
CFLAGS+=	-DHAVE_IPV6
CFLAGS+=	-D_THREAD_SAFE

CFLAGS+=	-I${.CURDIR}
CFLAGS+=	-I${.CURDIR}/mDNSCore
CFLAGS+=	-I${.CURDIR}/mDNSPosix
CFLAGS+=	-I${.CURDIR}/mDNSShared
CFLAGS+=	-I${.CURDIR}/DSO

# <servers/bootstrap.h> (bootstrap_check_in + bootstrap_port global)
# comes from liblaunch; its bootstrap.h pulls in Apple shim headers
# (AvailabilityMacros.h, ...) from launchd/freebsd-shims. Same -I
# pair hwregd / configd / ipconfigd use.
CFLAGS+=	-I${.CURDIR}/../launchd/liblaunch
CFLAGS+=	-I${.CURDIR}/../launchd/freebsd-shims

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

# -llaunch: bootstrap_check_in + the bootstrap_port global.
# -lsystem_kernel: mach_msg + the Mach port syscalls.
# -lpthread: uds_daemon spawns request threads; mDNSPosix uses
# pthread_mutex for the daemon-state lock.
LDADD+=		-llaunch -lsystem_kernel -lpthread

BINDIR=		/usr/sbin

.include <bsd.prog.mk>
