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

# kext.subproj builds libkext.a (Apple's OSKext engine, #182) first; kextdeps
# links it (Phase 1: dependency resolution). The minimal kld trio (#183) stays
# until the OSKext-backed tools graduate. Order matters: libkext before kextdeps.
SUBDIR=	kext.subproj kextload kextstat kextunload kextdeps

.include <bsd.subdir.mk>
