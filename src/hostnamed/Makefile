# hostnamed — Apple-shape persistent hostname decision daemon
# (freebsd-launchd-mach iter 3c).
#
# Sources:
#   hostnamed.c            — main(), event loop, signal handling, xlog.
#   prefs_monitor.c        — SCPrefs → SCDS Setup: keys bridge.
#   freebsd-shim/shim.c    — ip_plugin.h externs, SCPrivate SPI subset,
#                            freebsd_synthesize_hostname, synthesis
#                            machinery.
#   vendored/set-hostname.c — Apple's decision engine, APSL 2.0,
#                            sync'd from apple-oss-distributions/configd
#                            Plugins/IPMonitor/set-hostname.c.
#
# Build:   cd src/hostnamed && make SYSROOT=/path/to/rootfs
# Install: make DESTDIR=/path/to/rootfs install
#
# Plan: https://pkgdemon.github.io/freebsd-hostnamed-plan.html
# Issue: #63

PROG=		hostnamed
# MAN= (empty) suppresses bsd.prog.mk's default hostnamed.1 build;
# NO_MAN=yes alone is insufficient. Same suppression hwregd / configd /
# ipconfigd use.
MAN=
SRCS=		hostnamed.c prefs_monitor.c shim.c set-hostname.c

# WARNS=0: hostnamed.c pulls the CoreFoundation + SystemConfiguration
# public header tree. At WARNS>0 FreeBSD adds -Wsystem-headers -Werror
# which promotes warnings inside the vendored CF headers into hard
# errors. Same carry libSystemConfiguration / IPConfiguration use.
WARNS=		0
NO_MAN=		yes

# -fblocks: CoreFoundation public headers (CFCalendarPriv.h,
# ForSwiftFoundationOnly.h) declare block-typed APIs and refuse to
# compile without it. Same flag IPConfiguration / configd use.
CFLAGS+=	-O2 -pipe -fblocks
CFLAGS+=	-Wall -Wextra -Wshadow -Wstrict-prototypes
CFLAGS+=	-Wmissing-prototypes -Wpointer-arith

# SYSROOT include path FIRST so real Apple-source headers (dns_sd.h
# from libdns_sd, IOKit, ...) shadow the empty stubs that
# launchd/freebsd-shims carries for launchctl's dead includes — iter 3b
# added a real <dns_sd.h> dependency, and the launchd/freebsd-shims
# /dns_sd.h stub would otherwise win the include-path race.
SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

# liblaunch / launchd-shims pulled for parity with other daemons even
# though hostnamed doesn't call bootstrap_check_in — the CF umbrella
# transitively wants the same shim availability macros. libnotify
# freebsd-shims provides os/base.h, which notify.h includes for the
# Apple os/object.h ABI; notifyd / syslogd take the same -I path.
CFLAGS+=	-I${.CURDIR}/../launchd/liblaunch
CFLAGS+=	-I${.CURDIR}/../launchd/freebsd-shims
CFLAGS+=	-I${.CURDIR}/../Libnotify/freebsd-shims

# freebsd-shim — supplies ip_plugin.h, SCPrivate.h, SCValidation.h,
# SCDynamicStoreCopyDHCPInfo.h, netdb_async.h that the vendored Apple
# set-hostname.c expects to find. Listed BEFORE the launchd-shims path
# so the SCPrivate.h shim wins over any launchctl-era stub at the same
# include path.
CFLAGS+=	-I${.CURDIR}/freebsd-shim

.PATH:		${.CURDIR}
.PATH:		${.CURDIR}/freebsd-shim
.PATH:		${.CURDIR}/vendored

# -lSystemConfiguration: SCDynamicStoreCreate / SCDynamicStoreSetValue
# / SCErrorString — the configd publish surface.
# -lCoreFoundation: CFDictionary / CFString / CFNumber, dictionary
# shape we build for Setup:/System + Setup:/Network/HostNames.
# -lnotify: notify_post("com.apple.system.hostname") — Apple-canonical
# hostname-change broadcast.
# -ldns_sd: libdns_sd client API — DNSServiceQueryRecord for the iter 3b
# mDNS PTR tier. The library is built by src/mDNSResponder/libdns_sd
# and installed to /usr/lib/system/libdns_sd.so.1 alongside dns_sd.h.
# -ldispatch -lBlocksRuntime: transitively pulled by the SC umbrella
# (SCDynamicStoreSetDispatchQueue ABI surface).
# -lsystem_kernel: kenv(2) syscall trampoline + the FreeBSD libc base
# functions hostnamed uses (sysctl, getifaddrs, sethostname).
LDADD+=		-lsystem_kernel
LDADD+=		-lSystemConfiguration -lCoreFoundation
LDADD+=		-lnotify
LDADD+=		-ldns_sd
LDADD+=		-ldispatch -lBlocksRuntime -lpthread

BINDIR=		/usr/sbin

.include <bsd.prog.mk>
