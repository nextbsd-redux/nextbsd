# hostnamed — freebsd-launchd-mach hostname synthesis + publish daemon.
#
# iter 1: one-shot daemon. Synthesizes a hostname at boot via kenv +
# SMBIOS + NIC MAC + kern.hostuuid; publishes to SCDynamicStore +
# kernel + notifyd. Replaces FreeBSD's "Amnesiac" placeholder when
# the launchd boot path skips /etc/rc.conf.
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
SRCS=		hostnamed.c

# WARNS=0: hostnamed.c pulls the CoreFoundation + SystemConfiguration
# public header tree. At WARNS>0 FreeBSD adds -Wsystem-headers -Werror
# which promotes warnings inside the vendored CF headers into hard
# errors. Same carry libSystemConfiguration / IPConfiguration use.
WARNS=		0
NO_MAN=		yes

CFLAGS+=	-O2 -pipe
CFLAGS+=	-Wall -Wextra -Wshadow -Wstrict-prototypes
CFLAGS+=	-Wmissing-prototypes -Wpointer-arith

# liblaunch / freebsd-shims pulled for parity with other daemons even
# though iter 1 doesn't call bootstrap_check_in — the CF umbrella
# transitively wants the same shim availability macros.
CFLAGS+=	-I${.CURDIR}/../launchd/liblaunch
CFLAGS+=	-I${.CURDIR}/../launchd/freebsd-shims

SYSROOT?=
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif

# -lSystemConfiguration: SCDynamicStoreCreate / SCDynamicStoreSetValue
# / SCErrorString — the configd publish surface.
# -lCoreFoundation: CFDictionary / CFString / CFNumber, dictionary
# shape we build for Setup:/System + Setup:/Network/HostNames.
# -lnotify: notify_post("com.apple.system.hostname") — Apple-canonical
# hostname-change broadcast.
# -ldispatch -lBlocksRuntime: transitively pulled by the SC umbrella
# (SCDynamicStoreSetDispatchQueue ABI surface).
# -lsystem_kernel: kenv(2) syscall trampoline + the FreeBSD libc base
# functions hostnamed uses (sysctl, getifaddrs, sethostname).
LDADD+=		-lsystem_kernel
LDADD+=		-lSystemConfiguration -lCoreFoundation
LDADD+=		-lnotify
LDADD+=		-ldispatch -lBlocksRuntime -lpthread

BINDIR=		/usr/sbin

.include <bsd.prog.mk>
