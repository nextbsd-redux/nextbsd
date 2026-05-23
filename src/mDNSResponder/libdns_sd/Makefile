# libdns_sd — client library for Apple's DNS Service Discovery API.
#
# iter 3 of the mDNSResponder port. Compiles the cross-platform DNS-SD
# client stubs from mDNSShared/ into /usr/lib/system/libdns_sd.so so
# Apple-shape apps (gershwin desktop, ported Cocoa apps) can link
# -ldns_sd and call DNSServiceRegister / DNSServiceBrowse /
# DNSServiceResolve / DNSServiceQueryRecord / etc. The stubs speak
# the Apple AF_UNIX wire protocol against /var/run/mDNSResponder,
# which the daemon (PosixDaemon.c + uds_daemon.c, shipped iter 2)
# listens on.
#
# No Mach surface here — Apple's open-source mDNSResponder dropped
# the legacy Mach RPC layer years ago in favour of the cross-platform
# AF_UNIX protocol. The com.apple.mDNSResponder Mach service in our
# repo is currently a presence beacon (bootstrap_check_in only),
# not a DNS-SD endpoint. iter 4+ can revisit if a Mach-RPC client
# surfaces.
#
# Public header: /usr/include/dns_sd.h. Shared lib: /usr/lib/system/
# libdns_sd.so(.1). Same path scheme libIOKit / libSystemConfiguration
# use.

LIB=		dns_sd
SHLIB_MAJOR=	1

SRCS=		dnssd_clientlib.c dnssd_clientstub.c dnssd_ipc.c dnssd_errstring.c

.PATH:		${.CURDIR}/../mDNSShared

# WARNS=0: vendored Apple source isn't WARNS=6-clean (same trade
# the daemon's Makefile makes).
WARNS=		0
NO_MAN=		yes

# Mirror upstream's CFLAGS_COMMON + FreeBSD-specific defines.
CFLAGS+=	-O2 -pipe -fwrapv
CFLAGS+=	-DPOSIX_BUILD
CFLAGS+=	-DMDNS_UDS_SERVERPATH=\"/var/run/mDNSResponder\"
CFLAGS+=	-DMDNS_NO_STRICT=1
CFLAGS+=	-DHAVE_IPV6
CFLAGS+=	-D_THREAD_SAFE

CFLAGS+=	-I${.CURDIR}/../mDNSShared
CFLAGS+=	-I${.CURDIR}/../mDNSShared/utilities
CFLAGS+=	-I${.CURDIR}/../mDNSCore

INCS=		${.CURDIR}/../mDNSShared/dns_sd.h
INCSDIR=	${PREFIX}/include
LIBDIR=		${PREFIX}/lib/system

PREFIX?=	/usr

.include <bsd.lib.mk>
