# kext_tools — minimal kld-backed kextload / kextstat / kextunload.
#
# NextBSD proof-of-concept trio (nextbsd#183): prove a FreeBSD .ko wrapped
# as an Apple .kext bundle loads via an Apple-shaped tool. kextload/kextunload
# parse the bundle with CFBundle (libCoreFoundation, built at build.sh step
# 3p) and drive the load through the FreeBSD kld syscalls; kextstat is pure
# kld. No OSKext, no codesign, no IOKitPersonalities — the faithful
# kext_tools/OSKext port is tracked in nextbsd#182.
#
# Build:   cd src/kext_tools && make
# Install: make DESTDIR=/path/to/rootfs SYSROOT=/path/to/rootfs install

SUBDIR=	kextload kextstat kextunload

.include <bsd.subdir.mk>
