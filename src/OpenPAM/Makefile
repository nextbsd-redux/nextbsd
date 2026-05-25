# OpenPAM — Apple's PAM framework, vendored for freebsd-launchd-mach.
#
# Vendored from apple-oss-distributions/OpenPAM @ OpenPAM-35
# (commit b28c0250a25e553b4596179122a530a2203434c1). Upstream tree
# preserved under openpam/, compat/, pam.d/. Apple's xcodeproj /
# OpenPAM.plist are kept for traceability but not used by this build.
#
# Iter 1 deliverable (issue #93):
#   /usr/lib/libpam.so + libpam.so.6     — framework
#   /usr/lib/pam/pam_unix.so             — UNIX password verification
#   /usr/lib/pam/pam_deny.so             — always-deny terminator
#   /usr/lib/pam/pam_permit.so           — always-permit terminator
#   /usr/include/security/*.h            — public headers (7 files)
#
# Replaces the FreeBSD-pam-lib package (which provides libpam.so.6
# from FreeBSD's OpenPAM upstream). Same on-wire framework, same
# module ABI — FreeBSD-pam's remaining modules still load.
#
# Build:   make SYSROOT=/path/to/rootfs
# Install: make DESTDIR=/path/to/rootfs PREFIX=/usr install

CC ?=		cc

DESTDIR ?=
PREFIX ?=	/usr
LIBDIR =	${DESTDIR}${PREFIX}/lib
PAMDIR =	${DESTDIR}${PREFIX}/lib/pam
INCDIR =	${DESTDIR}${PREFIX}/include/security

# config.h shipped in openpam/ is autoconf-generated for the macOS
# build (HAVE_DLFCN_H, HAVE_FPURGE, HAVE_INTTYPES_H, …). All of
# those are also true on FreeBSD 15, so we use it as-is. If any
# HAVE_* mismatch surfaces, override here rather than regenerating
# via autoreconf.

CFLAGS_COMMON =	-fPIC -O2 -pipe \
		-Wno-deprecated-declarations \
		-Wno-unused-parameter \
		-Wno-unused-result \
		-I${.CURDIR}/openpam \
		-I${.CURDIR}/openpam/include \
		-I${.CURDIR}/openpam/lib \
		-DHAVE_CONFIG_H \
		-DLT_OBJDIR='"libs/"'

SYSROOT ?=
.if !empty(SYSROOT)
CFLAGS_COMMON +=	-I${SYSROOT}/usr/include
LDFLAGS_COMMON +=	-L${SYSROOT}/usr/lib \
			-Wl,-rpath,/usr/lib \
			-Wl,--allow-shlib-undefined
.endif

# Framework: every .c under openpam/lib/ except the static-linking-
# only helper, which is only relevant when consumers link against
# libpam.a (we ship .so only). .PATH lets bmake find foo.c when
# building foo.o in the current dir.
.PATH:		${.CURDIR}/openpam/lib

LIBPAM_SRCS != ls ${.CURDIR}/openpam/lib/openpam_*.c \
		    ${.CURDIR}/openpam/lib/pam_*.c \
		   | grep -v openpam_static.c
LIBPAM_OBJS = ${LIBPAM_SRCS:T:.c=.o}

# SHLIB major: match FreeBSD-pam-lib's libpam.so.6 exactly so our
# file overwrites theirs in-place. Existing FreeBSD-runtime binaries
# (/usr/bin/login, su, sshd, passwd, chsh, /rescue/login, ...) all
# record DT_NEEDED libpam.so.6 in their ELF headers; same soname
# means they transparently load OUR library, which is the whole
# point of iter 1's ABI verification.
LIBPAM_SONAME =	libpam.so.6

all:	${LIBPAM_SONAME} pam_unix.so pam_deny.so pam_permit.so

${LIBPAM_SONAME}: ${LIBPAM_OBJS}
	${CC} ${LDFLAGS_COMMON} -shared -Wl,-soname,${LIBPAM_SONAME} \
	    -o ${.TARGET} ${LIBPAM_OBJS}

# Per-source compile. .OBJDIR doesn't apply here — we compile from
# the source tree to the current dir. Bmake's transformation rule
# `.c.o:` handles each lib/*.c via the LIBPAM_SRCS dependency.
.SUFFIXES: .c .o
.c.o:
	${CC} ${CFLAGS_COMMON} -c -o ${.TARGET} ${.IMPSRC}

# Module loader entry points are in each pam_<name>/pam_<name>.c —
# single-file builds, link against libpam.so via -lpam if needed,
# but the modules themselves don't call back into libpam, so plain
# -shared is sufficient.
pam_unix.so:	${.CURDIR}/openpam/modules/pam_unix/pam_unix.c
	${CC} ${CFLAGS_COMMON} ${LDFLAGS_COMMON} \
	    -shared -o ${.TARGET} ${.ALLSRC}

pam_deny.so:	${.CURDIR}/openpam/modules/pam_deny/pam_deny.c
	${CC} ${CFLAGS_COMMON} ${LDFLAGS_COMMON} \
	    -shared -o ${.TARGET} ${.ALLSRC}

pam_permit.so:	${.CURDIR}/openpam/modules/pam_permit/pam_permit.c
	${CC} ${CFLAGS_COMMON} ${LDFLAGS_COMMON} \
	    -shared -o ${.TARGET} ${.ALLSRC}

install: all
	install -d ${LIBDIR} ${PAMDIR} ${INCDIR}
	install -m 0755 ${LIBPAM_SONAME} ${LIBDIR}/
	ln -sf ${LIBPAM_SONAME} ${LIBDIR}/libpam.so
	install -m 0755 pam_unix.so pam_deny.so pam_permit.so ${PAMDIR}/
	install -m 0644 ${.CURDIR}/openpam/include/security/openpam.h \
	                ${.CURDIR}/openpam/include/security/openpam_attr.h \
	                ${.CURDIR}/openpam/include/security/openpam_version.h \
	                ${.CURDIR}/openpam/include/security/pam_appl.h \
	                ${.CURDIR}/openpam/include/security/pam_constants.h \
	                ${.CURDIR}/openpam/include/security/pam_modules.h \
	                ${.CURDIR}/openpam/include/security/pam_types.h \
	                ${INCDIR}/

clean:
	rm -f ${LIBPAM_OBJS} ${LIBPAM_SONAME} pam_unix.so pam_deny.so pam_permit.so
