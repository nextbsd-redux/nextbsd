# Shared flags for the kext_tools trio. Auto-included by each sub-Makefile's
# bsd.prog.mk (from ${.CURDIR}/../Makefile.inc). The CF block must match how
# libCoreFoundation was built so the CF headers parse identically (see
# launchctl's Makefile / the ICU audit). kextstat is pure kld and ignores
# the CF link (it sets no LDADD); the CFLAGS are harmless to it.

BINDIR?=	/usr/sbin
MAN=			# no man pages — bsd.prog.mk otherwise defaults MAN=${PROG}.1
NO_MAN=		yes
WARNS?=		3

CFLAGS+=	-fblocks -fconstant-cfstrings
CFLAGS+=	-D__HAS_APPLE_ICU__=1 -DU_DISABLE_RENAMING=1 -DAPPLE_ICU_CHANGES=1
.if !empty(SYSROOT)
CFLAGS+=	-I${SYSROOT}/usr/include		# CoreFoundation/, dispatch/, ...
LDFLAGS+=	-L${SYSROOT}/usr/lib/system
LDFLAGS+=	-Wl,-rpath,/usr/lib/system
# libCoreFoundation.so carries undefined refs (libm: floor/fmod/ceil/scalbn,
# and CF-internal _CFThreadSetName/_CFGetCurrentDirectory) that the strict
# default --no-allow-shlib-undefined rejects; they resolve at runtime (libm
# arrives via the CF/ICU DT_NEEDED chain). launchctl links CF the same way
# and uses the same workaround.
LDFLAGS+=	-Wl,--allow-shlib-undefined
.endif
