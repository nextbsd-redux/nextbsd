# pam_modules — Apple's PAM module set, vendored for freebsd-launchd-mach.
#
# Vendored from apple-oss-distributions/pam_modules @ pam_modules-217.100.6.
# Upstream tree preserved (modules/, common/, tests/,
# test_pam_localauthentication/, pam_modules.xcodeproj/) for traceability.
# Apple's xcodeproj is kept but unused by this build.
#
# Iter 2 deliverable (issue #95): build 5 standalone modules that don't
# depend on OD / SecurityServer / SmartCard / OS-private subsystems
# we don't have:
#
#   /usr/lib/pam_self.so.6      — allow root to su to anyone
#   /usr/lib/pam_rootok.so.6    — allow root without password
#   /usr/lib/pam_uwtmp.so.6     — utmp/wtmp accounting
#   /usr/lib/pam_nologin.so.6   — respect /etc/nologin
#   /usr/lib/pam_env.so.6       — set environment variables
#
# All 5 install over FreeBSD-pam's same-named files (same flat
# /usr/lib/ layout iter 1 established for pam_unix/pam_deny/pam_permit).
#
# The other 12 modules in Apple's tarball (pam_aks, pam_basesystem,
# pam_group, pam_krb5, pam_launchd, pam_localauthentication, pam_mount,
# pam_ntlm, pam_opendirectory, pam_sacl, pam_smartcard, pam_tid)
# require Apple-private substrate we don't have — left in the source
# tree for future iters but not compiled.
#
# Build:   make SYSROOT=/path/to/rootfs
# Install: make DESTDIR=/path/to/rootfs PREFIX=/usr install

CC ?=		cc

DESTDIR ?=
PREFIX ?=	/usr
PAMDIR =	${DESTDIR}${PREFIX}/lib

# common/Logging.h is included by every module. By default (without
# -DPAM_USE_OS_LOG) it falls through to a syslog-based path with no
# Apple-private deps.
#
# OpenPAM headers (security/pam_*.h) and our codesign.h / dyld_priv.h
# shims live in the sibling src/OpenPAM/ tree (iter 1).
CFLAGS_COMMON =	-fPIC -O2 -pipe -fblocks \
		-Wno-deprecated-declarations \
		-Wno-unused-parameter \
		-Wno-unused-result \
		-I${.CURDIR}/common \
		-I${.CURDIR}/../OpenPAM/openpam/include \
		-I${.CURDIR}/../OpenPAM/freebsd-shims \
		-DLT_OBJDIR='"libs/"' \
		-DUTMPX_AUTOFILL_MASK=0

SYSROOT ?=
.if !empty(SYSROOT)
CFLAGS_COMMON +=	-I${SYSROOT}/usr/include
LDFLAGS_COMMON +=	-L${SYSROOT}/usr/lib \
			-L${SYSROOT}/usr/lib/system \
			-Wl,-rpath,/usr/lib \
			-Wl,-rpath,/usr/lib/system \
			-Wl,--allow-shlib-undefined
.endif

# Module link deps. Same as OpenPAM iter 1's bundled modules:
# -lpam for OpenPAM helper symbols; -lcrypt for any module that
# happens to call crypt(3) (FreeBSD keeps crypt in libcrypt, not
# libc). pam_self/pam_rootok/pam_nologin don't actually call crypt
# but linking it is harmless and keeps the build uniform.
PAM_MOD_LIBS =	-lpam -lcrypt

all:	pam_self.so pam_rootok.so pam_uwtmp.so pam_nologin.so pam_env.so

pam_self.so:	${.CURDIR}/modules/pam_self/pam_self.c
	${CC} ${CFLAGS_COMMON} ${LDFLAGS_COMMON} \
	    -shared -o ${.TARGET} ${.ALLSRC} ${PAM_MOD_LIBS}

pam_rootok.so:	${.CURDIR}/modules/pam_rootok/pam_rootok.c
	${CC} ${CFLAGS_COMMON} ${LDFLAGS_COMMON} \
	    -shared -o ${.TARGET} ${.ALLSRC} ${PAM_MOD_LIBS}

pam_uwtmp.so:	${.CURDIR}/modules/pam_uwtmp/pam_uwtmp.c
	${CC} ${CFLAGS_COMMON} ${LDFLAGS_COMMON} \
	    -shared -o ${.TARGET} ${.ALLSRC} ${PAM_MOD_LIBS}

pam_nologin.so:	${.CURDIR}/modules/pam_nologin/pam_nologin.c
	${CC} ${CFLAGS_COMMON} ${LDFLAGS_COMMON} \
	    -shared -o ${.TARGET} ${.ALLSRC} ${PAM_MOD_LIBS}

pam_env.so:	${.CURDIR}/modules/pam_env/pam_env.c
	${CC} ${CFLAGS_COMMON} ${LDFLAGS_COMMON} \
	    -shared -o ${.TARGET} ${.ALLSRC} ${PAM_MOD_LIBS}

install: all
	install -d ${PAMDIR}
	install -m 0755 pam_self.so ${PAMDIR}/pam_self.so.6
	install -m 0755 pam_rootok.so ${PAMDIR}/pam_rootok.so.6
	install -m 0755 pam_uwtmp.so ${PAMDIR}/pam_uwtmp.so.6
	install -m 0755 pam_nologin.so ${PAMDIR}/pam_nologin.so.6
	install -m 0755 pam_env.so ${PAMDIR}/pam_env.so.6

clean:
	rm -f pam_self.so pam_rootok.so pam_uwtmp.so \
	      pam_nologin.so pam_env.so
