# kext_tools — OSKext-backed kextload / kextunload + kextdeps, and the
# kld-only kextstat.
#
# kextload/kextunload/kextdeps link libkext.a (Apple's OSKext engine, #182):
# dependency-ordered load, bundle-aware unload, dependency resolution. kextstat
# stays pure kld (kldnext/kldstat) — its data comes from kld either way, so the
# OSKext query would add no fidelity. No codesign. kextd (#217) pushes each
# bundle's IOKitPersonalities into the in-kernel IOCatalogue (#215) at boot.
#
# Because libkext consumers link libIOKit, the whole subdir is built by build.sh
# AFTER libIOKit is installed (step 3r4a), not at the old pre-libIOKit point.
# kext.subproj (libkext.a, INTERNALLIB) is listed first so it builds before its
# consumers.
#
# Build:   cd src/kext_tools && make SYSROOT=/path/to/rootfs
# Install: make DESTDIR=/path/to/rootfs SYSROOT=/path/to/rootfs all install

SUBDIR=	kext.subproj kextload kextstat kextunload kextdeps kextd

.include <bsd.subdir.mk>
