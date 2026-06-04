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

# NOTE: kext.subproj (libkext.a) and kextdeps (the OSKext dependency-resolution
# CLI, #182 Phase 1) are intentionally NOT in this SUBDIR. kextdeps links
# libIOKit, which is installed LATER than this trio in build.sh (step 3q1),
# whereas libIOKit lands at 3r4 — so they are built by their own build.sh step
# (3r4a, after libIOKit) + a dependency-resolution smoke (#196), not here.
# To build them standalone against a sysroot:
#   make -C kext.subproj SYSROOT=<root> && make -C kextdeps SYSROOT=<root>

.include <bsd.subdir.mk>
