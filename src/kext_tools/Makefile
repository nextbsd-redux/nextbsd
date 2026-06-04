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
# CLI, #182 Phase 1) are deliberately NOT in SUBDIR yet. kextdeps links
# libIOKit, which isn't installed in the rootfs at the point build.sh builds
# kext_tools, and shipping a mid-port dev tool in the image is premature. They
# are wired in (with the build reordered after libIOKit, + a proper test) when
# the OSKext-backed tools graduate. To build them standalone against a sysroot:
#   make -C kext.subproj SYSROOT=<root> && make -C kextdeps SYSROOT=<root>

.include <bsd.subdir.mk>
