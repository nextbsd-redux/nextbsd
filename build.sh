#!/bin/sh
# build.sh — assemble a bootable FreeBSD GPT disk image for the launchd
# + Mach IPC port. The image has a read-write UFS root (no cd9660, no
# uzip, no unionfs): the kernel mounts the freebsd-ufs partition
# directly and execs /sbin/launchd as PID 1. Boots BIOS and UEFI.
# Runs on FreeBSD (host or vmactions VM). Produces out/disk.img.gz.
#
# Base comes from pkgbase (pkg.freebsd.org/FreeBSD:<major>:<arch>/
# base_latest), curated via pkglist-base.txt — no full base.txz /
# kernel.txz extraction. Userland gershwin/GNUstep stack is not built
# here; to be added back via a separate mechanism.

set -eu

: "${FREEBSD_VERSION:=15.0}"
: "${COMPRESS:=zstd}"
: "${LABEL:=LIVECD}"
ARCH=${ARCH:-amd64}

# pkgbase ABI: pkg.freebsd.org organizes repos under FreeBSD:<major>:<arch>.
# Strip any minor version (15.0 -> 15) so the URL resolves.
PKG_MAJOR=${FREEBSD_VERSION%%.*}
PKG_ABI="FreeBSD:${PKG_MAJOR}:${ARCH}"

ROOT=$(cd "$(dirname "$0")" && pwd)
WORK=$ROOT/work
OUT=$ROOT/out
DIST=$ROOT/distfiles
PKG_CONFIG=$WORK/pkg-config

MIRROR="https://download.freebsd.org/ftp/releases/${ARCH}/${FREEBSD_VERSION}-RELEASE"

mkdir -p "$WORK" "$OUT" "$DIST"

# Clean any prior partial build (but keep distfiles cached).
# A prior run's $WORK/rootfs holds pkgbase base-system libs that carry
# the schg (system-immutable) flag, so plain rm fails on re-runs; clear
# file flags first. No-op on a fresh CI VM where $WORK is empty.
chflags -R noschg "$WORK" 2>/dev/null || true
rm -rf "$WORK"/* "$OUT"/*

echo "==> build: FreeBSD $FREEBSD_VERSION ($ARCH), compress=$COMPRESS"

#
# 1. fetch src.txz.
#
# Base/kernel come from pkgbase (pkg.freebsd.org/${PKG_ABI}/base_latest)
# at step 2 — no more base.txz / kernel.txz.
#
# src.txz is still fetched on the host for two consumers:
#   - kernel sources for the mach.ko out-of-tree build (3b)
#   - mkisoimages.sh release script at step 11
# Neither lives inside the chroot.
#
if [ ! -f "$DIST/src.txz" ]; then
    echo "==> downloading src.txz"
    fetch -o "$DIST/src.txz" "$MIRROR/src.txz"
fi

#
# 2. install curated pkgbase set into rootfs staging dir.
#
# Replaces the wholesale `tar -xJf base.txz + kernel.txz` with a
# host-driven `pkg -R config -r rootfs install -y ...` against
# pkg.freebsd.org/${PKG_ABI}/base_latest. The package list is hand
# curated (pkglist-base.txt for runtime; buildpkgs-base.txt for build-
# only FreeBSD-* like clang/lld/-dev, purged before mkuzip alongside
# buildpkgs.txt so build tooling never ships in the ISO).
#
# ABI/OSVERSION/IGNORE_OSVERSION env let us install a ${PKG_ABI} target
# from a host pkg of any version. Same trick gershwin-on-freebsd uses.
#
# Note: cap_mkdb / pwd_mkdb are no longer needed here — the FreeBSD-rc
# / FreeBSD-runtime pkg post-install scripts rebuild login.conf.db and
# pwd.db / spwd.db automatically.
#
echo "==> writing pkgbase repo config (${PKG_ABI}, base_latest)"
mkdir -p "$PKG_CONFIG" "$WORK/rootfs"
cat > "$PKG_CONFIG/FreeBSD-base.conf" <<EOF
FreeBSD-base: {
  url: "https://pkg.freebsd.org/${PKG_ABI}/base_latest",
  enabled: yes
}
EOF

BASE_PKGS=$(     grep -v '^[[:space:]]*#' "$ROOT/pkglist-base.txt"   2>/dev/null | grep -v '^[[:space:]]*$' || true)
BASE_BUILD_PKGS=$(grep -v '^[[:space:]]*#' "$ROOT/buildpkgs-base.txt" 2>/dev/null | grep -v '^[[:space:]]*$' || true)

if [ -z "$BASE_PKGS" ]; then
    echo "ERROR: pkglist-base.txt is empty; refusing to build empty rootfs" >&2
    exit 1
fi

# Single combined install — runtime + build-only — so the dep solver
# resolves once. buildpkgs-base.txt entries get pkg deleted at the end of
# step 3 alongside buildpkgs.txt.
echo "==> installing pkgbase runtime + build pkgs into $WORK/rootfs"
echo "$BASE_PKGS" | sed 's/^/    runtime  /'
echo "$BASE_BUILD_PKGS" | sed 's/^/    build    /'
# shellcheck disable=SC2086
env ABI="${PKG_ABI}" \
    OSVERSION="${PKG_MAJOR}00000" \
    IGNORE_OSVERSION=yes \
    ASSUME_ALWAYS_YES=yes \
    pkg -R "$PKG_CONFIG" -r "$WORK/rootfs" install -y -r FreeBSD-base \
        $BASE_PKGS $BASE_BUILD_PKGS

#
# 3. chroot: install runtime pkgs (pkglist.txt) + build pkgs (buildpkgs.txt)
#    inside the chroot, then purge buildpkgs.txt + buildpkgs-base.txt
#    before the slim/mkuzip pass so build tooling doesn't ship in the ISO.
#    Build pkgs are installed and kept available for any chroot-side build
#    work; only the install/purge plumbing lives here — no inline build
#    recipe (the previous gershwin/GNUstep build was removed).
#
RUNTIME_PKGS=$(grep -v '^[[:space:]]*#' "$ROOT/pkglist.txt"   2>/dev/null | grep -v '^[[:space:]]*$' || true)
BUILD_PKGS=$(  grep -v '^[[:space:]]*#' "$ROOT/buildpkgs.txt" 2>/dev/null | grep -v '^[[:space:]]*$' || true)

if [ -n "$RUNTIME_PKGS" ] || [ -n "$BUILD_PKGS" ]; then
    cp /etc/resolv.conf "$WORK/rootfs/etc/resolv.conf"
    mount -t devfs devfs "$WORK/rootfs/dev"
    cleanup_chroot() {
        umount -f "$WORK/rootfs/dev" 2>/dev/null || true
        rm -f "$WORK/rootfs/etc/resolv.conf"
    }
    trap cleanup_chroot EXIT INT TERM

    chroot "$WORK/rootfs" env ASSUME_ALWAYS_YES=yes IGNORE_OSVERSION=yes pkg bootstrap -f

    if [ -n "$RUNTIME_PKGS" ]; then
        echo "==> installing runtime packages:"
        echo "$RUNTIME_PKGS" | sed 's/^/    /'
        # shellcheck disable=SC2086
        chroot "$WORK/rootfs" env \
            ASSUME_ALWAYS_YES=yes \
            IGNORE_OSVERSION=yes \
            LICENSES_ACCEPTED=NVIDIA \
            pkg install -y $RUNTIME_PKGS
    fi

    if [ -n "$BUILD_PKGS" ]; then
        echo "==> installing build packages:"
        echo "$BUILD_PKGS" | sed 's/^/    /'
        # shellcheck disable=SC2086
        chroot "$WORK/rootfs" env ASSUME_ALWAYS_YES=yes IGNORE_OSVERSION=yes \
            pkg install -y $BUILD_PKGS
    fi

    # Build pkgs (cmake/ninja/clang/etc.) stay installed through the
    # subsequent build steps (mach.ko, libsystem_kernel, libdispatch).
    # Purge + chroot cleanup move to the very end of the build phase,
    # after libdispatch is built (see "3z" below). Don't call
    # cleanup_chroot here — devfs + resolv.conf must stay live for the
    # libdispatch chroot build to work.
fi

#
# 3a. extract src.txz to $WORK/freebsd-src. Used for two things in
#     subsequent steps:
#       - kernel sources for the mach.ko out-of-tree build (3b)
#       - FreeBSD's release scripts (mkisoimages.sh) at step 11
#     One extraction, two consumers.
#
echo "==> extracting src.txz for kernel sources + release scripts"
mkdir -p "$WORK/freebsd-src"
tar -xJf "$DIST/src.txz" -C "$WORK/freebsd-src"

#
# 3b. build mach.ko against the freshly-extracted kernel sources and
#     install it into $WORK/rootfs/boot/kernel/mach.ko so it ships
#     inside rootfs.uzip. Step 8 below also copies it onto the cd9660
#     so the loader can preload it before init runs.
#
echo "==> building mach.ko"
"$ROOT/make-mach-kmod.sh" \
    --sysdir="$WORK/freebsd-src/usr/src/sys" \
    --prefix="$WORK/rootfs"
ls -lh "$WORK/rootfs/boot/kernel/mach.ko"

#
# 3c. build libsystem_kernel (formerly libmach) on the host and install
#     it into the chroot under the spike's chosen Apple-Libsystem layout:
#       /usr/lib/system/libsystem_kernel.so + .so.0 sonname symlink
#       /usr/include/mach/mach_traps.h
#       /usr/libdata/pkgconfig/libsystem_kernel.pc
#
echo "==> building libsystem_kernel (src/libmach)"
# bsd.lib.mk's install doesn't auto-create LIBDIR / INCSDIR / FILESDIR;
# pre-create them since /usr/lib/system is our convention (not a
# stock FreeBSD path) so pkgbase doesn't ship it.
mkdir -p "$WORK/rootfs/usr/lib/system" \
         "$WORK/rootfs/usr/include/mach" \
         "$WORK/rootfs/usr/libdata/pkgconfig"
make -C "$ROOT/src/libmach" \
    DESTDIR="$WORK/rootfs" \
    PREFIX=/usr \
    all install
ls -lh "$WORK/rootfs/usr/lib/system/libsystem_kernel.so" \
       "$WORK/rootfs/usr/include/mach/mach_traps.h" \
       "$WORK/rootfs/usr/libdata/pkgconfig/libsystem_kernel.pc"

# Prime /var/run/ld-elf.so.hints inside rootfs.uzip so a fresh boot
# already knows about /usr/lib/system. We do NOT register the path in
# /etc/ld-elf.so.conf -- our binaries (launchd, launchctl, libsystem_*
# consumers) all bake -Wl,-rpath,/usr/lib/system at link time, matching
# Apple's macOS convention where LC_RPATH load commands make each
# binary self-describing and ld-elf.so.conf-style global registries
# aren't used. The -m primer here only helps any bare dlopen("lib*.so")
# call that doesn't carry rpath context (e.g. future third-party
# Apple-stack components we vendor that aren't yet rpath-correct).
echo "==> priming ldconfig hints for /usr/lib/system"
chroot "$WORK/rootfs" ldconfig -m /usr/lib /usr/lib/system

#
# 3d. build the libsystem_kernel smoke test binary, install to
#     /usr/tests/freebsd-launchd-mach/test_libmach. CI's run.sh invokes
#     it post-login to verify rtld resolution + Mach roundtrip.
#
echo "==> building test_libmach"
mkdir -p "$WORK/rootfs/usr/tests/freebsd-launchd-mach"
cc -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/test_libmach" \
   "$ROOT/src/mach_kmod/tests/test_libmach.c" \
   -lsystem_kernel
ls -lh "$WORK/rootfs/usr/tests/freebsd-launchd-mach/test_libmach"

# test_mach_port — exercises the three port-management traps wired
# for the libxpc port (mach_port_allocate / _insert_right / _deallocate).
# Runs after test_libmach in CI's run.sh; failure surfaces as
# MACH-PORT-FAIL.
echo "==> building test_mach_port"
cc -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/test_mach_port" \
   "$ROOT/src/mach_kmod/tests/test_mach_port.c" \
   -lsystem_kernel
ls -lh "$WORK/rootfs/usr/tests/freebsd-launchd-mach/test_mach_port"

# test_task_special_port — exercises task_get_special_port /
# task_set_special_port traps (Phase G prerequisite for the bootstrap
# server's port discovery). Failure surfaces as TASK-SPECIAL-PORT-FAIL.
echo "==> building test_task_special_port"
cc -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/test_task_special_port" \
   "$ROOT/src/mach_kmod/tests/test_task_special_port.c" \
   -lsystem_kernel
ls -lh "$WORK/rootfs/usr/tests/freebsd-launchd-mach/test_task_special_port"

# test_host_bootstrap — Phase G2b validation: host_set_special_port
# stores into realhost.special[HOST_BOOTSTRAP_PORT], and
# task_get_special_port(TASK_BOOTSTRAP_PORT) falls back to it when
# the per-task itk_bootstrap slot is null. HOST-BOOTSTRAP-FAIL
# surfaces in run.sh on regression.
echo "==> building test_host_bootstrap"
cc -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/test_host_bootstrap" \
   "$ROOT/src/mach_kmod/tests/test_host_bootstrap.c" \
   -lsystem_kernel
ls -lh "$WORK/rootfs/usr/tests/freebsd-launchd-mach/test_host_bootstrap"

# Install <servers/bootstrap.h> public header. libsystem_kernel's
# Makefile installs everything else under /usr/include/mach/; the
# bootstrap header sits at /usr/include/servers/bootstrap.h per
# Apple convention. Do it by hand here rather than wrestling with
# bsd.lib.mk INCS_GRP plumbing.
echo "==> installing <servers/bootstrap.h>"
mkdir -p "$WORK/rootfs/usr/include/servers"
cp "$ROOT/src/libmach/include/servers/bootstrap.h" \
   "$WORK/rootfs/usr/include/servers/bootstrap.h"

# test_bootstrap — Phase G1 single-task validation of the bootstrap
# protocol (check_in / look_up round-trip via libbootstrap +
# bootstrap_server_run in a pthread). libbootstrap.c is linked
# statically into the test binary for now; Phase G2 promotes it to
# a real /usr/lib/system/libbootstrap.so once the daemon lands.
echo "==> building test_bootstrap"
cc -I"$WORK/rootfs/usr/include" \
   -I"$ROOT/src/bootstrap" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/test_bootstrap" \
   "$ROOT/src/bootstrap/tests/test_bootstrap.c" \
   "$ROOT/src/bootstrap/libbootstrap.c" \
   -lsystem_kernel -lpthread
ls -lh "$WORK/rootfs/usr/tests/freebsd-launchd-mach/test_bootstrap"

# bootstrap_server — Phase G2c standalone daemon. Allocates the
# service port, publishes via host_set_special_port, runs the
# bootstrap_server_run loop until SIGTERM. Installs to
# /usr/sbin/ (matches the install-layout spike's system-daemon
# convention); run.sh starts/stops it for the smoke test.
echo "==> building bootstrap_server"
cc -I"$WORK/rootfs/usr/include" \
   -I"$ROOT/src/bootstrap" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system \
   -o "$WORK/rootfs/usr/sbin/bootstrap_server" \
   "$ROOT/src/bootstrap/bootstrap_server.c" \
   "$ROOT/src/bootstrap/libbootstrap.c" \
   -lsystem_kernel -lpthread
ls -lh "$WORK/rootfs/usr/sbin/bootstrap_server"

# test_bootstrap_remote — Phase G2d cross-process client. Validates
# that a fresh process finds the daemon via task_get_bootstrap_port
# (host fallback) and round-trips check_in/look_up over real Mach
# IPC. run.sh starts the daemon before this runs, kills it after.
echo "==> building test_bootstrap_remote"
cc -I"$WORK/rootfs/usr/include" \
   -I"$ROOT/src/bootstrap" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/test_bootstrap_remote" \
   "$ROOT/src/bootstrap/tests/test_bootstrap_remote.c" \
   "$ROOT/src/bootstrap/libbootstrap.c" \
   -lsystem_kernel -lpthread
ls -lh "$WORK/rootfs/usr/tests/freebsd-launchd-mach/test_bootstrap_remote"

#
# 3e. verify: assert install shape + ldconfig resolution + ldd resolves
#     the test binary's libsystem_kernel.so.0 dep. Build fails fast here
#     instead of producing a broken ISO.
#
echo "==> verifying libsystem_kernel install"
ls -la "$WORK/rootfs/usr/lib/system/" || true
# bsd.lib.mk installs .so.0 as the regular file (the actual library
# binary) and .so as the dev-time symlink pointing to it. Verify both
# shapes correctly.
test -f "$WORK/rootfs/usr/lib/system/libsystem_kernel.so.0" \
    || { echo "FAIL: libsystem_kernel.so.0 (the library binary) missing"; exit 1; }
test -L "$WORK/rootfs/usr/lib/system/libsystem_kernel.so" \
    || { echo "FAIL: libsystem_kernel.so (dev symlink to .so.0) missing"; exit 1; }
chroot "$WORK/rootfs" ldconfig -r | grep -q libsystem_kernel \
    || { echo "FAIL: ldconfig hints missing libsystem_kernel"; exit 1; }
chroot "$WORK/rootfs" ldd /usr/tests/freebsd-launchd-mach/test_libmach \
    | grep -q "libsystem_kernel.so.0 => /usr/lib/system/" \
    || { echo "FAIL: ldd doesn't resolve test_libmach to /usr/lib/system/"; exit 1; }
echo "==> libsystem_kernel install verified"

#
# 3e2 (task #39). Build/install mig EARLY — moved up from former step 3k
# so libdispatch's cmake can MIG-generate protocolServer.h from
# src/libdispatch/src/protocol.defs (HAVE_MACH path needs it).
# mig is self-contained (byacc + flex grammars only); no dependency
# on libdispatch, so the reorder is safe.
#
echo "==> building migcom (early, for libdispatch HAVE_MACH path)"
make -C "$ROOT/src/bootstrap_cmds/migcom.tproj" \
    DESTDIR="$WORK/rootfs" \
    BINDIR=/usr/libexec \
    all install
install -m 0755 "$ROOT/src/bootstrap_cmds/migcom.tproj/mig.sh" \
                "$WORK/rootfs/usr/bin/mig"
mkdir -p "$WORK/rootfs/usr/share/man/man1"
install -m 0644 "$ROOT/src/bootstrap_cmds/migcom.tproj/mig.1" \
                "$WORK/rootfs/usr/share/man/man1/mig.1"
install -m 0644 "$ROOT/src/bootstrap_cmds/migcom.tproj/migcom.1" \
                "$WORK/rootfs/usr/share/man/man1/migcom.1"
chroot "$WORK/rootfs" /usr/bin/mig -version \
    || { echo "FAIL: mig -version exited non-zero (early install)"; exit 1; }
echo "==> mig install verified (early)"

#
# 3f. build libdispatch from src/libdispatch (vendored swift-corelibs-
#     libdispatch + FreeBSD perf patch). cmake/ninja build inside the
#     chroot using buildpkgs already installed (clang, lld, cmake,
#     ninja). Installs:
#       /usr/lib/system/libdispatch.so + .so.0 (the lib)
#       /usr/lib/system/libBlocksRuntime.so + .so.0 (bundled — replaces
#         the dropped FreeBSD-libblocksruntime pkg; same upstream Apple
#         compiler-rt source)
#       /usr/include/dispatch/*.h
#       /usr/include/os/*.h
#       /usr/include/Block.h + Block_private.h
#     Plus libsystem_dispatch / libsystem_blocks symlinks for Apple-
#     canonical naming (per install-layout-spike §4 + §15). Proper
#     SONAME rename to libsystem_dispatch is a future cleanup; symlinks
#     keep both -ldispatch and -lsystem_dispatch link lines working.
#
echo "==> copying src/libdispatch to chroot"
mkdir -p "$WORK/rootfs/tmp/libdispatch"
cp -a "$ROOT/src/libdispatch/." "$WORK/rootfs/tmp/libdispatch/"

echo "==> building libdispatch in chroot"
chroot "$WORK/rootfs" /bin/sh -ex <<'CHROOT_DISPATCH'
# Chroot inherits caller's PATH which may not include /usr/local/bin
# where pkg installs cmake / ninja. Set explicitly.
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
mkdir -p /tmp/libdispatch-build
cd /tmp/libdispatch-build
cmake -G Ninja /tmp/libdispatch \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib/system \
    -DINSTALL_DISPATCH_HEADERS_DIR=/usr/include/dispatch \
    -DINSTALL_BLOCK_HEADERS_DIR=/usr/include \
    -DINSTALL_OS_HEADERS_DIR=/usr/include/os \
    -DINSTALL_PRIVATE_HEADERS=ON \
    -DHAVE_MACH:BOOL=ON \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Release
ninja
ninja install
CHROOT_DISPATCH

echo "==> creating libdispatch / libsystem_dispatch / libsystem_blocks symlinks"
# libdispatch's CMakeLists doesn't set SOVERSION — output is just
# libdispatch.so (unversioned), no .so.0. DT_SONAME is empty, so
# consumers' DT_NEEDED becomes the filename (libdispatch.so).
#
# Issue: FreeBSD's ldconfig only indexes libs matching lib*.so.[0-9]+
# in its hints DB. An unversioned lib*.so is on disk but invisible to
# rtld via the hints lookup. Programs without RPATH/RUNPATH would fail
# to dlopen libdispatch by sonname.
#
# Workaround: create .so.0 symlinks pointing at the unversioned file so
# ldconfig's glob matches. Cleaner long-term fix: patch libdispatch's
# CMakeLists to set SOVERSION + OUTPUT_NAME=system_dispatch.
ln -sf libdispatch.so      "$WORK/rootfs/usr/lib/system/libdispatch.so.0"
ln -sf libBlocksRuntime.so "$WORK/rootfs/usr/lib/system/libBlocksRuntime.so.0"
ln -sf libdispatch.so      "$WORK/rootfs/usr/lib/system/libsystem_dispatch.so"
ln -sf libdispatch.so      "$WORK/rootfs/usr/lib/system/libsystem_dispatch.so.0"
ln -sf libBlocksRuntime.so "$WORK/rootfs/usr/lib/system/libsystem_blocks.so"
ln -sf libBlocksRuntime.so "$WORK/rootfs/usr/lib/system/libsystem_blocks.so.0"

# Re-prime ldconfig hints now that libdispatch + BlocksRuntime are
# installed at /usr/lib/system.
chroot "$WORK/rootfs" ldconfig -m /usr/lib /usr/lib/system

# Cleanup chroot build artifacts.
rm -rf "$WORK/rootfs/tmp/libdispatch" "$WORK/rootfs/tmp/libdispatch-build"

#
# 3g. build the libdispatch smoke test binary, install to
#     /usr/tests/freebsd-launchd-mach/test_libdispatch.
#
echo "==> building test_libdispatch"
cc -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/test_libdispatch" \
   "$ROOT/src/libdispatch-tests/test_libdispatch.c" \
   -ldispatch -lpthread

echo "==> building test_bsd_logger (Phase J runtime)"
cc -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/test_bsd_logger" \
   "$ROOT/tests/test_bsd_logger.c"

echo "==> building test_libdispatch_mach"
# Links libsystem_kernel for the userland mach_reply_port / mach_msg
# trap shims the round-trip test uses to allocate a receive port and
# self-send through the kernel.
cc -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/test_libdispatch_mach" \
   "$ROOT/src/libdispatch-tests/test_libdispatch_mach.c" \
   -ldispatch -lsystem_kernel -lpthread

#
# 3h. verify libdispatch install shape + ldconfig + ldd resolution.
#
echo "==> verifying libdispatch install"
ls -la "$WORK/rootfs/usr/lib/system/" || true
# libdispatch installs unversioned: libdispatch.so is the actual file
# (no .so.0). Same for libBlocksRuntime.
test -f "$WORK/rootfs/usr/lib/system/libdispatch.so" \
    || { echo "FAIL: libdispatch.so (the library binary) missing"; exit 1; }
test -L "$WORK/rootfs/usr/lib/system/libsystem_dispatch.so" \
    || { echo "FAIL: libsystem_dispatch.so symlink missing"; exit 1; }
test -f "$WORK/rootfs/usr/lib/system/libBlocksRuntime.so" \
    || { echo "FAIL: libBlocksRuntime.so missing"; exit 1; }
test -L "$WORK/rootfs/usr/lib/system/libsystem_blocks.so" \
    || { echo "FAIL: libsystem_blocks.so symlink missing"; exit 1; }
test -f "$WORK/rootfs/usr/include/Block.h" \
    || { echo "FAIL: /usr/include/Block.h missing (libdispatch should ship it)"; exit 1; }
test -f "$WORK/rootfs/usr/include/dispatch/dispatch.h" \
    || { echo "FAIL: /usr/include/dispatch/dispatch.h missing"; exit 1; }
chroot "$WORK/rootfs" ldconfig -r | grep -q libdispatch \
    || { echo "FAIL: ldconfig hints missing libdispatch"; exit 1; }
chroot "$WORK/rootfs" ldd /usr/tests/freebsd-launchd-mach/test_libdispatch \
    | grep -q "libdispatch.so => /usr/lib/system/" \
    || { echo "FAIL: ldd doesn't resolve test_libdispatch to /usr/lib/system/"; exit 1; }
echo "==> libdispatch install verified"

#
# 3i. build libxpc (src/libxpc, vendored from ravynOS lib/libxpc/).
#     Apple-canonical XPC userland library, ported. Depends on
#     libsystem_kernel (Mach trap shims) and libdispatch (queues +
#     DISPATCH_SOURCE_TYPE_MACH_RECV). The bootstrap protocol is
#     vendored in directly (../bootstrap/libbootstrap.c is in the
#     Makefile SRCS) so libxpc.so resolves bootstrap_check_in /
#     bootstrap_look_up internally without needing a separate
#     libbootstrap.so on disk.
#
#     Host build via bsd.lib.mk — same pattern as libsystem_kernel.
#     Install:
#       /usr/lib/system/libxpc.so + libxpc.so.4 (the lib)
#       /usr/include/xpc/{activity,base,connection,debug,endpoint,
#                         launchd,xpc}.h
#
# Install <mach/mach.h> umbrella header NOW — deliberately deferred
# from libsystem_kernel's INCS install so it isn't present during the
# libdispatch build (libdispatch's CMake __has_include(<mach/mach.h>)
# detection would otherwise turn on HAVE_MACH and try to compile Apple-
# internal Mach paths that don't exist in our stack).
echo "==> installing <mach/mach.h> umbrella header"
cp "$ROOT/src/libmach/include/mach/mach.h" \
   "$WORK/rootfs/usr/include/mach/mach.h"

# Apple-shim headers — xpc/xpc.h, xpc/base.h, etc. transitively include
# <uuid/uuid.h>, <Availability.h>, and <launch.h>. Inside libxpc's own
# build these resolve via -I${.CURDIR}; external consumers
# (test_libxpc and the future launchd / configd) only see
# /usr/include, so install the same shims there.
#   uuid/uuid.h   — redirect to FreeBSD's <uuid.h>
#   Availability.h — expand Apple availability macros to nothing
#   launch.h       — vendored ravynOS liblaunch public surface
echo "==> installing Apple-shim public headers"
mkdir -p "$WORK/rootfs/usr/include/uuid"
cp "$ROOT/src/libxpc/uuid/uuid.h" \
   "$WORK/rootfs/usr/include/uuid/uuid.h"
cp "$ROOT/src/libxpc/Availability.h" \
   "$WORK/rootfs/usr/include/Availability.h"
cp "$ROOT/src/libxpc/launch.h" \
   "$WORK/rootfs/usr/include/launch.h"

echo "==> building libxpc (src/libxpc)"
mkdir -p "$WORK/rootfs/usr/include/xpc"
# SYSROOT tells libxpc's Makefile where to find headers + libs of the
# in-build rootfs. Don't pass CFLAGS / LDFLAGS directly — bmake treats
# command-line variable assignments as overrides (not appends), which
# would clobber the Makefile's CFLAGS+= additions.
make -C "$ROOT/src/libxpc" \
    DESTDIR="$WORK/rootfs" \
    PREFIX=/usr \
    SYSROOT="$WORK/rootfs" \
    all install
ls -lh "$WORK/rootfs/usr/lib/system/libxpc.so" \
       "$WORK/rootfs/usr/include/xpc/xpc.h"

# Re-prime ldconfig hints now that libxpc is installed at /usr/lib/system.
chroot "$WORK/rootfs" ldconfig -m /usr/lib /usr/lib/system

# Verify libxpc install: shape + ldconfig hint + ldd-resolution of
# its libdispatch / libsystem_kernel deps. Fails fast if missing.
echo "==> verifying libxpc install"
test -f "$WORK/rootfs/usr/lib/system/libxpc.so.4" \
    || { echo "FAIL: libxpc.so.4 missing"; exit 1; }
test -L "$WORK/rootfs/usr/lib/system/libxpc.so" \
    || { echo "FAIL: libxpc.so dev symlink missing"; exit 1; }
test -f "$WORK/rootfs/usr/include/xpc/xpc.h" \
    || { echo "FAIL: /usr/include/xpc/xpc.h missing"; exit 1; }
chroot "$WORK/rootfs" ldconfig -r | grep -q libxpc \
    || { echo "FAIL: ldconfig hints missing libxpc"; exit 1; }
echo "==> libxpc install verified"

#
# 3j. test_libxpc build moved to AFTER step 3m (liblaunch) because
# task #39 Path A migration left libxpc.so with unresolved
# bootstrap_check_in / bootstrap_port refs that liblaunch.so resolves.
# See the moved block below the liblaunch step.

#
# 3k. build bootstrap_cmds (src/bootstrap_cmds, vendored from Apple's
#     apple-oss-distributions/bootstrap_cmds). Ships Apple's Mach Interface
#     Generator (MIG):
#       /usr/bin/mig          — shell wrapper (mig.sh, FreeBSD-patched)
#       /usr/libexec/migcom   — compiler backend
#       /usr/share/man/man1/{mig,migcom}.1
#
#     Zero FreeBSD-base conflicts — no FreeBSD pkg ships mig or migcom.
#     Build deps from FreeBSD-base buildpkgs: FreeBSD-byacc + FreeBSD-flex
#     (parser.y + lexxer.l grammars). Both get purged in step 3z so MIG
#     itself remains on the ISO but the build tools don't.
#
#     This is a prerequisite for the (next-phase) launchd-842 port —
#     launchd-842/src/ ships seven .defs files whose Mach-RPC stubs are
#     code-generated by mig. We install mig now so the launchd-842 step
#     can shell out to it directly.
#
echo "==> mig install handled in early step 3e2 (above libdispatch)"

#
# 3l. Phase I1a — generate launchd-842's MIG RPC stubs.
#     launchd-842/src/ ships .defs files whose Mach-RPC client/server
#     stubs MIG code-generates. This step proves the mig we just built
#     can actually process Apple's .defs against our vendored MIG
#     type-system (mach/std_types.defs + mach_types.defs +
#     machine_types.defs, in src/libmach/include/mach/).
#
#     Output lands in $WORK/launchd-mig/ — pure build artifacts, not
#     rootfs content. The launchd daemon build (later I1c step) will
#     consume them. For now this is the I1a checkpoint: mig runs, the
#     expected .c/.h come out.
#
#     .defs handled: job, job_forward, job_reply, internal, helper,
#     mach_exc, notify. job_types.defs is a type-only include (no
#     subsystem) — pulled in by the others, not compiled standalone.
#     protocol_jobmgr.defs is deliberately skipped: it's NOT in Apple's
#     Xcode build and imports nonexistent bootstrap_public.h (stale,
#     like bootstrap_cmds' handler.c). mach_exc.defs / notify.defs are
#     SDK-provided on macOS; here they're vendored from ravynOS into
#     src/launchd/src/ — core.c needs mach_excServer.h, runtime.c needs
#     notifyServer.h.
#
echo "==> Phase I1a: generating launchd-842 MIG stubs"
MIG_OUT="$WORK/launchd-mig"
mkdir -p "$MIG_OUT"
# mig runs `cc -E` over each .defs; -I must reach our MIG type-system
# in src/libmach/include/mach/ plus launchd's own liblaunch/ headers
# (the .defs `import` lines reference vproc.h etc., though those only
# affect generated #include lines, not mig-time resolution).
MIG_INCS="-I$ROOT/src/libmach/include -I$ROOT/src/launchd/liblaunch -I$ROOT/src/launchd/src"
for d in job job_forward job_reply internal helper mach_exc notify; do
    defs="$ROOT/src/launchd/src/${d}.defs"
    echo "  mig: ${d}.defs"
    # -sheader emits ${d}Server.h — libvproc.c #includes helperServer.h
    # to drive a helper-downcall server via mach_msg_server_once().
    ( cd "$MIG_OUT" && \
      MIGCC=/usr/bin/cc MIGCOM="$WORK/rootfs/usr/libexec/migcom" \
      /bin/sh "$ROOT/src/bootstrap_cmds/migcom.tproj/mig.sh" \
        $MIG_INCS \
        -header "${d}.h" -user "${d}User.c" \
        -server "${d}Server.c" -sheader "${d}Server.h" \
        "$defs" ) \
      || { echo "FAIL: mig could not process ${d}.defs"; exit 1; }
done
# job.defs is the central one — assert its three outputs exist.
for f in job.h jobUser.c jobServer.c; do
    test -s "$MIG_OUT/$f" \
        || { echo "FAIL: mig produced no $f from job.defs"; exit 1; }
done
echo "==> Phase I1a: MIG stubs generated ($(ls "$MIG_OUT" | wc -l | tr -d ' ') files in $MIG_OUT)"
ls -la "$MIG_OUT"

#
# 3m. Phase I1b — build liblaunch (src/launchd/liblaunch).
#     Apple's launch_data IPC library — the foundation launchd +
#     launchctl both link against. Three TUs: liblaunch.c (launch_data
#     API + launch_msg socket IPC), libvproc.c (vproc handles +
#     helper-downcall server), libbootstrap.c (bootstrap client
#     wrappers). Host build via bsd.lib.mk, same pattern as
#     libsystem_kernel. MIGOUT points the build at the I1a stubs.
#
#     Install: /usr/lib/system/liblaunch.so + .so.1
#     Headers NOT installed yet — deferred to the I1c launchd build,
#     same way <mach/mach.h> was deferred past the libdispatch build.
#
echo "==> Phase I1b: building liblaunch (src/launchd/liblaunch)"
make -C "$ROOT/src/launchd/liblaunch" \
    DESTDIR="$WORK/rootfs" \
    PREFIX=/usr \
    MIGOUT="$MIG_OUT" \
    SYSROOT="$WORK/rootfs" \
    all install
ls -lh "$WORK/rootfs/usr/lib/system/liblaunch.so" \
       "$WORK/rootfs/usr/lib/system/liblaunch.so.1"
chroot "$WORK/rootfs" ldconfig -m /usr/lib /usr/lib/system
echo "==> Phase I1b: liblaunch built + installed"

#
# 3j (moved here). build test_libxpc — Phase H2 smoke check. Links
# libxpc.so so the in-process xpc_dictionary_* round-trip exercises
# the newly-installed library. Moved past liblaunch build because
# task #39 Path A made libxpc.so depend on liblaunch.so for the
# bootstrap_check_in / bootstrap_port symbols; rtld needs liblaunch
# in the dependency graph when libxpc is loaded.
#
echo "==> building test_libxpc (post-liblaunch)"
cc -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/test_libxpc" \
   "$ROOT/src/libxpc-tests/test_libxpc.c" \
   -lxpc -llaunch -ldispatch -lsystem_kernel -lpthread

chroot "$WORK/rootfs" ldd /usr/tests/freebsd-launchd-mach/test_libxpc \
    | grep -q "libxpc.so.* => /usr/lib/system/" \
    || { echo "FAIL: ldd doesn't resolve test_libxpc to /usr/lib/system/libxpc.so"; exit 1; }
echo "==> test_libxpc built + ldd verified"

#
# 3n. Phase I1c — launchd daemon moved AFTER libCoreFoundation step
#     (now at 3p1, see below). The daemon's launchd_plist_scan.c
#     links libCoreFoundation + lib_FoundationICU to parse
#     /System/Library/LaunchDaemons at PID-1 boot, so CF must be
#     installed in SYSROOT before launchd compiles.
#

#
# 3o. build swift-foundation-icu (src/swift-foundation-icu) -> libicucore.so.
#     Apple's slimmed ICU fork purpose-built for swift-corelibs-foundation.
#     CF's libCoreFoundation includes ICU headers via the private namespace
#     <_foundation_unicode/uloc.h> (11 .c files); this fork ships those
#     headers natively, no symlink alias required.
#
#     CMake build inside the chroot using cmake/ninja from buildpkgs.
#     Three vendored CMakeLists patches recorded in src/swift-foundation-icu/
#     PORTING_README.md: drop Swift from LANGUAGES, force U_DISABLE_RENAMING=1
#     (CF references unprefixed symbols), and use standard CMAKE_INSTALL_*
#     dirs instead of upstream's lib/swift/<system>/ SwiftPM nesting.
#
#     Installs:
#       /usr/lib/system/lib_FoundationICU.so   (the unified library)
#       /usr/lib/system/libicucore.so          (Apple-canonical alias)
#       /usr/include/_foundation_unicode/*.h      (212 headers)
#
#     Plan: pkgdemon.github.io/freebsd-libicu-port-plan.html
#
echo "==> copying src/swift-foundation-icu to chroot"
mkdir -p "$WORK/rootfs/tmp/swift-foundation-icu"
cp -a "$ROOT/src/swift-foundation-icu/." "$WORK/rootfs/tmp/swift-foundation-icu/"

echo "==> building swift-foundation-icu in chroot"
chroot "$WORK/rootfs" /bin/sh -ex <<'CHROOT_ICU'
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
mkdir -p /tmp/swift-foundation-icu-build
cd /tmp/swift-foundation-icu-build
cmake -G Ninja /tmp/swift-foundation-icu \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib/system \
    -DCMAKE_INSTALL_INCLUDEDIR=include \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Release
# Default ninja parallelism is fine now that the
# icu_packaged_data.cpp giant TU is replaced by a .incbin .S file
# (see icuSources/common/CMakeLists.txt's freebsd-launchd-mach
# patch). Compile peak across all CC/CXX TUs is back to typical
# ICU-source sizes (a few hundred MB each), comfortable on the
# 8 GB VM with default -jN.
ninja
ninja install
CHROOT_ICU

echo "==> creating libicucore.so alias (Apple-canonical name)"
# swift-foundation-icu's CMake produces lib_FoundationICU.so (the target
# name is _FoundationICU). Apple's macOS ships the same body of code as
# /usr/lib/libicucore.dylib. Create both names so callers can link with
# whichever they prefer (-l_FoundationICU or -licucore).
ln -sf lib_FoundationICU.so "$WORK/rootfs/usr/lib/system/libicucore.so"
ln -sf lib_FoundationICU.so "$WORK/rootfs/usr/lib/system/libicucore.so.74"
ln -sf lib_FoundationICU.so "$WORK/rootfs/usr/lib/system/lib_FoundationICU.so.74"

# Re-prime ldconfig hints now that libicucore is installed.
chroot "$WORK/rootfs" ldconfig -m /usr/lib /usr/lib/system

# Cleanup chroot build artifacts.
rm -rf "$WORK/rootfs/tmp/swift-foundation-icu" "$WORK/rootfs/tmp/swift-foundation-icu-build"

#
# 3o-verify. assert libicu install shape + ldd resolution.
#
echo "==> verifying libicu install"
ls -la "$WORK/rootfs/usr/lib/system/" | grep -E 'icu|FoundationICU' || true
test -f "$WORK/rootfs/usr/lib/system/lib_FoundationICU.so" \
    || { echo "FAIL: lib_FoundationICU.so (the library binary) missing"; exit 1; }
test -L "$WORK/rootfs/usr/lib/system/libicucore.so" \
    || { echo "FAIL: libicucore.so symlink missing"; exit 1; }
test -f "$WORK/rootfs/usr/include/_foundation_unicode/uloc.h" \
    || { echo "FAIL: /usr/include/_foundation_unicode/uloc.h missing (CF needs it)"; exit 1; }
test -f "$WORK/rootfs/usr/include/_foundation_unicode/uchar.h" \
    || { echo "FAIL: /usr/include/_foundation_unicode/uchar.h missing"; exit 1; }
test -f "$WORK/rootfs/usr/include/_foundation_unicode/ucal.h" \
    || { echo "FAIL: /usr/include/_foundation_unicode/ucal.h missing"; exit 1; }
chroot "$WORK/rootfs" ldconfig -r | grep -qE 'icucore|FoundationICU' \
    || { echo "FAIL: ldconfig hints missing libicucore / lib_FoundationICU"; exit 1; }
echo "==> libicu install verified"

#
# 3p. build libCoreFoundation (src/libCoreFoundation).
#     swift-corelibs-foundation's pure-C CF, built standalone with
#     DEPLOYMENT_RUNTIME_SWIFT=0 — no libswiftCore dep. 16 ICU-using
#     and 2 Windows-only .c files are dropped from SRCS per the ICU
#     audit at pkgdemon.github.io/freebsd-libcorefoundation-icu-audit.
#
#     Install: /usr/lib/system/libCoreFoundation.so.6 + headers at
#     /usr/include/CoreFoundation/.
#
echo "==> building libCoreFoundation (src/libCoreFoundation)"
# bsd.lib.mk's INCS install rule uses install(1) directly without an
# auto-mkdir of INCSDIR. Pre-create /usr/include/CoreFoundation/ in
# the rootfs so the install step succeeds.
mkdir -p "$WORK/rootfs/usr/include/CoreFoundation"
make -C "$ROOT/src/libCoreFoundation" \
    DESTDIR="$WORK/rootfs" \
    PREFIX=/usr \
    SYSROOT="$WORK/rootfs" \
    all install
ls -lh "$WORK/rootfs/usr/lib/system/libCoreFoundation.so" \
       "$WORK/rootfs/usr/lib/system/libCoreFoundation.so.6"
chroot "$WORK/rootfs" ldconfig -m /usr/lib /usr/lib/system
echo "==> libCoreFoundation built + installed"

#
# 3p. build test_corefoundation — smoke check that exercises CFDictionary
#     + CFString + CFPropertyList XML/binary round-trip. Links libCore-
#     Foundation.so.6 from /usr/lib/system; verifies the legacy
#     refcount path is alive and the plist driver works.
#
echo "==> building test_corefoundation"
# -fblocks: CF public headers (CFCalendarPriv.h, ForSwiftFoundationOnly.h)
# use ^block syntax in their function decls; clang errors out without
# -fblocks even when the consumer doesn't use blocks itself.
cc -fblocks \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/test_corefoundation" \
   "$ROOT/src/libCoreFoundation-tests/test_corefoundation.c" \
   -lCoreFoundation -ldispatch -lBlocksRuntime -lsystem_kernel -lpthread
chroot "$WORK/rootfs" ldd /usr/tests/freebsd-launchd-mach/test_corefoundation \
    | grep -q "libCoreFoundation.so.* => /usr/lib/system/" \
    || { echo "FAIL: ldd doesn't resolve test_corefoundation to /usr/lib/system/libCoreFoundation.so"; exit 1; }
echo "==> test_corefoundation built + ldd verified"

#
# 3p1. Phase I1c (moved): build the launchd daemon (src/launchd/src).
#      Was at step 3n earlier in the pipeline; moved here because
#      launchd_plist_scan.c now links libCoreFoundation +
#      lib_FoundationICU to parse /System/Library/LaunchDaemons at
#      PID-1 boot. CF must be installed in SYSROOT before launchd
#      compiles.
#
#      Seven hand-written TUs (launchd.c core.c runtime.c ipc.c log.c
#      kill2.c ktrace.c) plus launchd_plist_scan.c plus the MIG stubs
#      from step 3l. Host build via bsd.prog.mk; MIGOUT points at the
#      I1a output, SYSROOT at the in-build rootfs.
#
#      Install: /sbin/launchd — Apple-Unix path, matches the
#      install-layout spike.
#
echo "==> Phase I1c: building launchd (src/launchd/src) [post-CF]"
make -C "$ROOT/src/launchd/src" \
    DESTDIR="$WORK/rootfs" \
    MIGOUT="$MIG_OUT" \
    SYSROOT="$WORK/rootfs" \
    all install
ls -lh "$WORK/rootfs/sbin/launchd"
echo "==> Phase I1c: launchd built + installed"

#
# 3q. build launchctl (src/launchd/support/launchctl.c).
#     The user-side control utility for launchd. 4549 LOC of
#     CF-heavy code; depends on liblaunch + libCoreFoundation +
#     lib_FoundationICU + libxpc + libdispatch + libsystem_kernel.
#
#     Uses three new shims under src/launchd/freebsd-shims/:
#       IOKit/IOKitLib.h        — stubs IOKitWaitQuiet,
#                                 IORegistryEntryFromPath, etc.
#                                 (4 vestigial sites; would be
#                                 no-ops on FreeBSD even with
#                                 real IOKit — they query Apple-
#                                 specific IODeviceTree paths)
#       NSSystemDirectories.h   — enumerates /Local/Library and
#                                 /System/Library for LaunchAgents
#                                 / LaunchDaemons scan
#       mach-o/getsect.h        — stubs getsectiondata() to NULL
#                                 so the embedded XPC cache lookup
#                                 falls through to live-disk scan
#
#     Install: /bin/launchctl, matching Apple's shipping path.
#
echo "==> building launchctl (src/launchd/support)"
mkdir -p "$WORK/rootfs/bin"
make -C "$ROOT/src/launchd/support" \
    DESTDIR="$WORK/rootfs" \
    SYSROOT="$WORK/rootfs" \
    all install
ls -lh "$WORK/rootfs/bin/launchctl"
chroot "$WORK/rootfs" ldd /bin/launchctl \
    | grep -q "libCoreFoundation.so.* => /usr/lib/system/" \
    || { echo "FAIL: ldd doesn't resolve launchctl to /usr/lib/system/libCoreFoundation.so"; exit 1; }
chroot "$WORK/rootfs" ldd /bin/launchctl \
    | grep -q "lib_FoundationICU.so" \
    || { echo "FAIL: ldd doesn't resolve launchctl to lib_FoundationICU.so"; exit 1; }
echo "==> launchctl built + ldd verified"

#
# 3r. build hwregd (src/hwregd/hwregd.c).
#     freebsd-launchd-mach hardware registry daemon. Phase 0 iter 1:
#     pure libc daemon, no library deps. Reads /dev/devctl events
#     and logs them to /var/log/hwregd.log via the launchd plist's
#     StandardErrorPath. Future iters add the devmatch search_hints
#     parser, kldload-on-nomatch, and Mach pub/sub.
#     Install: /usr/sbin/hwregd. Smoke marker HWREGD-BUILD-OK.
#     Plan: pkgdemon.github.io/freebsd-hardware-registry-iokit-plan.html
#
echo "==> building hwregd (src/hwregd)"
mkdir -p "$WORK/rootfs/usr/sbin"
# Phase 1 iter 2a — generate the hwreg.defs MIG stubs (hwreg_server()
# demux for hwregd, hwregUser.c for the hwregquery test client). Same
# mig.sh + migcom path as the launchd MIG step above.
HWREG_MIG="$WORK/hwreg-mig"
mkdir -p "$HWREG_MIG"
( cd "$HWREG_MIG" && \
  MIGCC=/usr/bin/cc MIGCOM="$WORK/rootfs/usr/libexec/migcom" \
  /bin/sh "$ROOT/src/bootstrap_cmds/migcom.tproj/mig.sh" \
    -I"$ROOT/src/libmach/include" \
    -header hwreg.h -user hwregUser.c \
    -server hwregServer.c -sheader hwregServer.h \
    "$ROOT/src/hwregd/hwreg.defs" ) \
  || { echo "FAIL: mig could not process hwreg.defs"; exit 1; }
test -s "$HWREG_MIG/hwregServer.c" \
    || { echo "FAIL: mig produced no hwregServer.c"; exit 1; }
make -C "$ROOT/src/hwregd" \
    DESTDIR="$WORK/rootfs" \
    SYSROOT="$WORK/rootfs" \
    MIGOUT="$HWREG_MIG" \
    all install
ls -lh "$WORK/rootfs/usr/sbin/hwregd"
test -x "$WORK/rootfs/usr/sbin/hwregd" \
    || { echo "FAIL: /usr/sbin/hwregd not installed or not executable"; exit 1; }
echo "==> HWREGD-BUILD-OK"

# hwregtest — iter 3b-ii test client for hwregd's Mach pub/sub bus.
# run.sh subscribes with it and checks for the HWREG-PUBSUB-OK marker.
echo "==> building hwregtest"
mkdir -p "$WORK/rootfs/usr/tests/freebsd-launchd-mach"
cc -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/hwregtest" \
   "$ROOT/src/hwregd/hwregtest.c" \
   -llaunch -lsystem_kernel
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/hwregtest" \
    || { echo "FAIL: hwregtest not built"; exit 1; }

# hwregquery — iter 2a/2b test client for hwregd's Mach-RPC query API.
# Links the MIG user stub hwregUser.c (+ libxpc for the nvlist API the
# iter 2b property-bag / lookup routines use); run.sh exercises it and
# checks for the HWREG-RPC-OK marker.
echo "==> building hwregquery"
cc -I"$HWREG_MIG" -I"$ROOT/src/hwregd" -I"$ROOT/src/libxpc" \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/hwregquery" \
   "$ROOT/src/hwregd/hwregquery.c" "$HWREG_MIG/hwregUser.c" \
   -llaunch -lsystem_kernel -lxpc
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/hwregquery" \
    || { echo "FAIL: hwregquery not built"; exit 1; }

#
# 3r2. build configd (src/configd/) — configd iter 1.
#      configd hosts the SCDynamicStore over the MIG `config` Mach
#      subsystem (config.defs, base id 20000). iter 1 is the daemon
#      skeleton: checks com.apple.SystemConfiguration.configd in and
#      runs the config.defs receive loop with stub routine handlers.
#      Generate the config.defs MIG stubs (config_server() demux),
#      then build + install /usr/sbin/configd. Same mig.sh + migcom
#      path as the launchd / hwregd MIG steps above.
#      configd port plan: Mach-IPC track (not the sockets/DO plan).
#
echo "==> building configd (src/configd)"
CONFIGD_MIG="$WORK/configd-mig"
mkdir -p "$CONFIGD_MIG"
( cd "$CONFIGD_MIG" && \
  MIGCC=/usr/bin/cc MIGCOM="$WORK/rootfs/usr/libexec/migcom" \
  /bin/sh "$ROOT/src/bootstrap_cmds/migcom.tproj/mig.sh" \
    -I"$ROOT/src/libmach/include" \
    -header config.h -user configUser.c \
    -server configServer.c -sheader configServer.h \
    "$ROOT/src/configd/config.defs" ) \
  || { echo "FAIL: mig could not process config.defs"; exit 1; }
test -s "$CONFIGD_MIG/configServer.c" \
    || { echo "FAIL: mig produced no configServer.c"; exit 1; }
make -C "$ROOT/src/configd" \
    DESTDIR="$WORK/rootfs" \
    SYSROOT="$WORK/rootfs" \
    MIGOUT="$CONFIGD_MIG" \
    all install
ls -lh "$WORK/rootfs/usr/sbin/configd"
test -x "$WORK/rootfs/usr/sbin/configd" \
    || { echo "FAIL: /usr/sbin/configd not installed or not executable"; exit 1; }
echo "==> CONFIGD-BUILD-OK"

# configtest — configd iter 3 store round-trip test client. Speaks
# config.defs directly via the MIG user stub configUser.c (generated
# above). run.sh runs it and checks for the CONFIGD-STORE-OK marker.
echo "==> building configtest"
cc -I"$CONFIGD_MIG" -I"$ROOT/src/configd" \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/configtest" \
   "$ROOT/src/configd/configtest.c" "$CONFIGD_MIG/configUser.c" \
   -llaunch -lsystem_kernel
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/configtest" \
    || { echo "FAIL: configtest not built"; exit 1; }

# notifytest — configd iter 4 change-notification round-trip test
# client. Opens two sessions (per-session ports), watches a key,
# registers a Mach notification port, changes the key from the other
# session and confirms the notification + notifychanges. run.sh runs
# it and checks for the CONFIGD-NOTIFY-OK marker.
echo "==> building notifytest"
cc -I"$CONFIGD_MIG" -I"$ROOT/src/configd" \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/notifytest" \
   "$ROOT/src/configd/notifytest.c" "$CONFIGD_MIG/configUser.c" \
   -llaunch -lsystem_kernel
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/notifytest" \
    || { echo "FAIL: notifytest not built"; exit 1; }

# patterntest — configd iter 5 regex pattern-watch test client. Opens
# two sessions, watches a POSIX regex, changes a matching and a
# non-matching key from the other session and confirms configd
# notifies only for the match. run.sh runs it and checks for the
# CONFIGD-PATTERN-OK marker.
echo "==> building patterntest"
cc -I"$CONFIGD_MIG" -I"$ROOT/src/configd" \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/patterntest" \
   "$ROOT/src/configd/patterntest.c" "$CONFIGD_MIG/configUser.c" \
   -llaunch -lsystem_kernel
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/patterntest" \
    || { echo "FAIL: patterntest not built"; exit 1; }

# listtest — configd iter 6 key-listing test client. Stores keys and
# exercises configlist (prefix / empty-key / regex queries). run.sh
# runs it and checks for the CONFIGD-LIST-OK marker.
echo "==> building listtest"
cc -I"$CONFIGD_MIG" -I"$ROOT/src/configd" \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/listtest" \
   "$ROOT/src/configd/listtest.c" "$CONFIGD_MIG/configUser.c" \
   -llaunch -lsystem_kernel
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/listtest" \
    || { echo "FAIL: listtest not built"; exit 1; }

# multitest — configd iter 7 batch-routine test client. Exercises
# configset_m / configget_m / notifyset; links config_wire.c for the
# key-list / key-value payload encodings. run.sh runs it and checks
# for the CONFIGD-MULTI-OK marker.
echo "==> building multitest"
cc -I"$CONFIGD_MIG" -I"$ROOT/src/configd" \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/multitest" \
   "$ROOT/src/configd/multitest.c" "$ROOT/src/configd/config_wire.c" \
   "$CONFIGD_MIG/configUser.c" \
   -llaunch -lsystem_kernel
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/multitest" \
    || { echo "FAIL: multitest not built"; exit 1; }

#
# 3r3. build libSystemConfiguration (src/libSystemConfiguration/) —
#      the SystemConfiguration client framework, iter 1. Wraps configd's
#      config.defs MIG RPC in the CoreFoundation-typed SCDynamicStore*
#      API. Builds after configd (it reuses the configUser.c MIG client
#      stubs in $CONFIGD_MIG and configd's config_wire.c) and after
#      libCoreFoundation + liblaunch, the libraries it links against.
#      Installs /usr/lib/system/libSystemConfiguration.so + the public
#      headers at /usr/include/SystemConfiguration/.
#
echo "==> building libSystemConfiguration (src/libSystemConfiguration)"
# bsd.incs.mk installs INCS into INCSDIR but does not create it — make
# the header subdir first (build.sh does the same for libxpc's xpc/).
mkdir -p "$WORK/rootfs/usr/include/SystemConfiguration"
make -C "$ROOT/src/libSystemConfiguration" \
    DESTDIR="$WORK/rootfs" \
    SYSROOT="$WORK/rootfs" \
    MIGOUT="$CONFIGD_MIG" \
    all install
# Re-prime ldconfig hints now that libSystemConfiguration is installed.
chroot "$WORK/rootfs" ldconfig -m /usr/lib /usr/lib/system
ls -lh "$WORK/rootfs/usr/lib/system/libSystemConfiguration.so"
test -f "$WORK/rootfs/usr/lib/system/libSystemConfiguration.so.1" \
    || { echo "FAIL: libSystemConfiguration.so.1 not installed"; exit 1; }
test -L "$WORK/rootfs/usr/lib/system/libSystemConfiguration.so" \
    || { echo "FAIL: libSystemConfiguration.so dev symlink missing"; exit 1; }
test -f "$WORK/rootfs/usr/include/SystemConfiguration/SCDynamicStore.h" \
    || { echo "FAIL: SCDynamicStore.h header not installed"; exit 1; }
chroot "$WORK/rootfs" ldconfig -r | grep -q libSystemConfiguration \
    || { echo "FAIL: ldconfig hints missing libSystemConfiguration"; exit 1; }
echo "==> libSystemConfiguration built + installed"

# sctest — libSystemConfiguration iter 1 round-trip test client.
# Exercises the SCDynamicStore* CF API (create / set / get / add /
# remove / list) against the live configd. run.sh runs it and checks
# for the SC-STORE-OK marker.
echo "==> building sctest"
cc -fblocks \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/sctest" \
   "$ROOT/src/libSystemConfiguration/sctest.c" \
   -lSystemConfiguration -lCoreFoundation -lsystem_kernel -llaunch -lpthread
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/sctest" \
    || { echo "FAIL: sctest not built"; exit 1; }
chroot "$WORK/rootfs" ldd /usr/tests/freebsd-launchd-mach/sctest \
    | grep -q "libSystemConfiguration.so.* => /usr/lib/system/" \
    || { echo "FAIL: ldd doesn't resolve sctest to /usr/lib/system/libSystemConfiguration.so"; exit 1; }
echo "==> sctest built + ldd verified"

# scnotifytest — libSystemConfiguration iter 2 change-notification test
# client. One session watches a key + registers an SCDynamicStore
# callback on a dispatch queue; another writes the key; the callback
# must fire. run.sh runs it and checks for the SC-NOTIFY-OK marker.
echo "==> building scnotifytest"
cc -fblocks \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scnotifytest" \
   "$ROOT/src/libSystemConfiguration/scnotifytest.c" \
   -lSystemConfiguration -lCoreFoundation -ldispatch -lBlocksRuntime \
   -lsystem_kernel -llaunch -lpthread
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scnotifytest" \
    || { echo "FAIL: scnotifytest not built"; exit 1; }
echo "==> scnotifytest built"

# scrltest — libSystemConfiguration iter 3 run-loop-source test client.
# One session watches a key and adds a SCDynamicStoreCreateRunLoopSource
# to its run loop; another writes the key; running the run loop must
# fire the callback. run.sh runs it and checks for the SC-RUNLOOP-OK
# marker.
echo "==> building scrltest"
cc -fblocks \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scrltest" \
   "$ROOT/src/libSystemConfiguration/scrltest.c" \
   -lSystemConfiguration -lCoreFoundation -ldispatch -lBlocksRuntime \
   -lsystem_kernel -llaunch -lpthread
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scrltest" \
    || { echo "FAIL: scrltest not built"; exit 1; }
echo "==> scrltest built"

# scmultitest — libSystemConfiguration iter 4 batch get/set test client.
# Exercises SCDynamicStoreSetMultiple / SCDynamicStoreCopyMultiple
# against the live configd. run.sh runs it and checks for the
# SC-MULTI-OK marker.
echo "==> building scmultitest"
cc -fblocks \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scmultitest" \
   "$ROOT/src/libSystemConfiguration/scmultitest.c" \
   -lSystemConfiguration -lCoreFoundation -lsystem_kernel -llaunch -lpthread
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scmultitest" \
    || { echo "FAIL: scmultitest not built"; exit 1; }
echo "==> scmultitest built"

# scprefstest — libSystemConfiguration SCPreferences test client.
# Exercises the SCPreferences read / edit / commit cycle: set values,
# commit, re-open and confirm they persisted, list keys, remove. run.sh
# runs it and checks for the SC-PREFS-OK marker.
echo "==> building scprefstest"
cc -fblocks \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scprefstest" \
   "$ROOT/src/libSystemConfiguration/scprefstest.c" \
   -lSystemConfiguration -lCoreFoundation -lsystem_kernel -llaunch -lpthread
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scprefstest" \
    || { echo "FAIL: scprefstest not built"; exit 1; }
echo "==> scprefstest built"

# scpathtest — libSystemConfiguration SCPreferences path-accessor test
# client. Exercises SCPreferencesPathSetValue / PathGetValue /
# PathRemoveValue (nested '/'-separated paths). run.sh runs it and
# checks for the SC-PATH-OK marker.
echo "==> building scpathtest"
cc -fblocks \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scpathtest" \
   "$ROOT/src/libSystemConfiguration/scpathtest.c" \
   -lSystemConfiguration -lCoreFoundation -lsystem_kernel -llaunch -lpthread
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scpathtest" \
    || { echo "FAIL: scpathtest not built"; exit 1; }
echo "==> scpathtest built"

# sclocktest — libSystemConfiguration SCPreferences lock test client.
# Exercises SCPreferencesLock / SCPreferencesUnlock contention between
# two sessions, plus commit's internal auto-lock. run.sh runs it and
# checks for the SC-LOCK-OK marker.
echo "==> building sclocktest"
cc -fblocks \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/sclocktest" \
   "$ROOT/src/libSystemConfiguration/sclocktest.c" \
   -lSystemConfiguration -lCoreFoundation -lsystem_kernel -llaunch -lpthread
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/sclocktest" \
    || { echo "FAIL: sclocktest not built"; exit 1; }
echo "==> sclocktest built"

# scprefsnotifytest — libSystemConfiguration SCPreferences change-
# notification test client. One session watches a preferences file on
# a dispatch queue, another commits a change, and the callback must
# fire. run.sh runs it and checks for the SC-PNOTIFY-OK marker.
echo "==> building scprefsnotifytest"
cc -fblocks \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scprefsnotifytest" \
   "$ROOT/src/libSystemConfiguration/scprefsnotifytest.c" \
   -lSystemConfiguration -lCoreFoundation -ldispatch -lBlocksRuntime \
   -lsystem_kernel -llaunch -lpthread
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scprefsnotifytest" \
    || { echo "FAIL: scprefsnotifytest not built"; exit 1; }
echo "==> scprefsnotifytest built"

# scplinktest — libSystemConfiguration SCNetworkConfiguration iter 1
# test client. Exercises the SCPreferences path link accessors that
# SCNetworkConfiguration is built on: SCPreferencesPathCreateUnique
# Child, SCPreferencesPathSet/GetLink, and __LINK__ resolution (leaf
# and mid-path) by SCPreferencesPathGetValue. run.sh runs it and checks
# for the SC-PLINK-OK marker.
echo "==> building scplinktest"
cc -fblocks \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scplinktest" \
   "$ROOT/src/libSystemConfiguration/scplinktest.c" \
   -lSystemConfiguration -lCoreFoundation -lsystem_kernel -llaunch -lpthread
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scplinktest" \
    || { echo "FAIL: scplinktest not built"; exit 1; }
echo "==> scplinktest built"

# scnetiftest — libSystemConfiguration SCNetworkConfiguration iter 2
# test client. Exercises SCNetworkInterfaceCopyAll + the interface
# accessors: the QEMU guest boots with one e1000 NIC, so the
# enumeration must report an Ethernet interface with a hardware
# address and exclude loopback. run.sh runs it and checks for the
# SC-NETIF-OK marker.
echo "==> building scnetiftest"
cc -fblocks \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scnetiftest" \
   "$ROOT/src/libSystemConfiguration/scnetiftest.c" \
   -lSystemConfiguration -lCoreFoundation -lsystem_kernel -llaunch -lpthread
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scnetiftest" \
    || { echo "FAIL: scnetiftest not built"; exit 1; }
echo "==> scnetiftest built"

# scnetsvctest — libSystemConfiguration SCNetworkConfiguration iter 3
# test client. Creates an SCNetworkService on the e1000 interface,
# names it, attaches + configures an IPv4 SCNetworkProtocol, commits,
# reopens and confirms it persisted, then tears it down. run.sh runs
# it and checks for the SC-NETSVC-OK marker.
echo "==> building scnetsvctest"
cc -fblocks \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scnetsvctest" \
   "$ROOT/src/libSystemConfiguration/scnetsvctest.c" \
   -lSystemConfiguration -lCoreFoundation -lsystem_kernel -llaunch -lpthread
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scnetsvctest" \
    || { echo "FAIL: scnetsvctest not built"; exit 1; }
echo "==> scnetsvctest built"

# scnetsettest — libSystemConfiguration SCNetworkConfiguration iter 4
# test client. Creates an SCNetworkSet, names it, adds + removes a
# service, sets a service order, makes the set current, commits,
# reopens and confirms it persisted. run.sh runs it and checks for the
# SC-NETSET-OK marker.
echo "==> building scnetsettest"
cc -fblocks \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scnetsettest" \
   "$ROOT/src/libSystemConfiguration/scnetsettest.c" \
   -lSystemConfiguration -lCoreFoundation -lsystem_kernel -llaunch -lpthread
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scnetsettest" \
    || { echo "FAIL: scnetsettest not built"; exit 1; }
echo "==> scnetsettest built"

# scvlantest — libSystemConfiguration SCNetworkConfiguration iter 5
# test client. Creates an SCVLANInterface on the e1000 interface,
# checks its physical interface / tag / name / options, commits,
# reopens and confirms it persisted, then removes it. run.sh runs it
# and checks for the SC-VLAN-OK marker.
echo "==> building scvlantest"
cc -fblocks \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scvlantest" \
   "$ROOT/src/libSystemConfiguration/scvlantest.c" \
   -lSystemConfiguration -lCoreFoundation -lsystem_kernel -llaunch -lpthread
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scvlantest" \
    || { echo "FAIL: scvlantest not built"; exit 1; }
echo "==> scvlantest built"

# scbondtest — libSystemConfiguration SCNetworkConfiguration iter 6
# test client. Creates an SCBondInterface, adds the e1000 interface as
# a member, checks the member list / name / options / availability,
# commits, reopens and confirms it persisted, then removes it. run.sh
# runs it and checks for the SC-BOND-OK marker.
echo "==> building scbondtest"
cc -fblocks \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scbondtest" \
   "$ROOT/src/libSystemConfiguration/scbondtest.c" \
   -lSystemConfiguration -lCoreFoundation -lsystem_kernel -llaunch -lpthread
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scbondtest" \
    || { echo "FAIL: scbondtest not built"; exit 1; }
echo "==> scbondtest built"

# scbridgetest — libSystemConfiguration SCNetworkConfiguration iter 7
# test client. Creates an SCBridgeInterface, adds the e1000 interface
# as a member, checks the member list / name / options / the
# AllowConfiguredMembers flag, commits, reopens and confirms it
# persisted, then removes it. run.sh runs it and checks for the
# SC-BRIDGE-OK marker.
echo "==> building scbridgetest"
cc -fblocks \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scbridgetest" \
   "$ROOT/src/libSystemConfiguration/scbridgetest.c" \
   -lSystemConfiguration -lCoreFoundation -lsystem_kernel -llaunch -lpthread
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/scbridgetest" \
    || { echo "FAIL: scbridgetest not built"; exit 1; }
echo "==> scbridgetest built"

#
# 3r4. build libIOKit (src/libIOKit/) — the IOKit userland facade,
#      iter 1. Thin CF wrapper over hwregd's MIG hwreg.defs Mach RPC
#      (K1 of the IOKit-userland port plan). iter 1 ships the read-
#      only registry walk (IORegistryGetRootEntry, GetChildIterator,
#      IOIteratorNext, GetName, GetPath, IOObject{Retain,Release}).
#      Re-uses hwregd's MIG client stubs in $HWREG_MIG.
#      Installs /usr/lib/system/libIOKit.so + /usr/include/IOKit/.
#      Plan: pkgdemon.github.io/freebsd-hardware-registry-iokit-plan.html
#
echo "==> building libIOKit (src/libIOKit)"
# bsd.incs.mk installs INCS into INCSDIR but does not create it —
# match the same pre-mkdir libSystemConfiguration / libxpc use.
mkdir -p "$WORK/rootfs/usr/include/IOKit"
make -C "$ROOT/src/libIOKit" \
    DESTDIR="$WORK/rootfs" \
    SYSROOT="$WORK/rootfs" \
    MIGOUT="$HWREG_MIG" \
    all install
# Re-prime ldconfig hints now that libIOKit is installed.
chroot "$WORK/rootfs" ldconfig -m /usr/lib /usr/lib/system
ls -lh "$WORK/rootfs/usr/lib/system/libIOKit.so"
test -f "$WORK/rootfs/usr/lib/system/libIOKit.so.1" \
    || { echo "FAIL: libIOKit.so.1 not installed"; exit 1; }
test -L "$WORK/rootfs/usr/lib/system/libIOKit.so" \
    || { echo "FAIL: libIOKit.so dev symlink missing"; exit 1; }
test -f "$WORK/rootfs/usr/include/IOKit/IOKitLib.h" \
    || { echo "FAIL: IOKit/IOKitLib.h header not installed"; exit 1; }
chroot "$WORK/rootfs" ldconfig -r | grep -q libIOKit \
    || { echo "FAIL: ldconfig hints missing libIOKit"; exit 1; }
echo "==> libIOKit built + installed"

# iokittest — libIOKit iter 1 walk test client. Walks the hwregd
# registry through the IOKit facade and prints IOKIT-WALK-OK. run.sh
# runs it and checks for the marker.
# -fblocks: IOKitLib.h pulls CoreFoundation; CF headers carry block
# typedefs that need -fblocks even when iokittest itself doesn't use
# any. Same flag the libSystemConfiguration test clients carry.
echo "==> building iokittest"
cc -fblocks \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/iokittest" \
   "$ROOT/src/libIOKit/iokittest.c" \
   -lIOKit -lCoreFoundation -lsystem_kernel -llaunch -lpthread
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/iokittest" \
    || { echo "FAIL: iokittest not built"; exit 1; }
chroot "$WORK/rootfs" ldd /usr/tests/freebsd-launchd-mach/iokittest \
    | grep -q "libIOKit.so.* => /usr/lib/system/" \
    || { echo "FAIL: ldd doesn't resolve iokittest to /usr/lib/system/libIOKit.so"; exit 1; }
echo "==> iokittest built + ldd verified"

# iokitmatchtest — libIOKit iter 2 properties + matching test client.
# Pulls a node's property bag as a CFDictionary, fetches a single
# property as a CFString, exercises IOServiceMatching +
# IOServiceGetMatchingService(s) against PCIDevice (deterministic in
# the QEMU guest), prints IOKIT-MATCH-OK. run.sh runs it and checks
# for the marker.
echo "==> building iokitmatchtest"
cc -fblocks \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/iokitmatchtest" \
   "$ROOT/src/libIOKit/iokitmatchtest.c" \
   -lIOKit -lCoreFoundation -lsystem_kernel -llaunch -lpthread
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/iokitmatchtest" \
    || { echo "FAIL: iokitmatchtest not built"; exit 1; }
echo "==> iokitmatchtest built"

# ioreg(8) — libIOKit iter 3 registry introspection tool. Walks the
# hwregd registry via the IOKit facade and prints the tree (+
# properties with -l), modelled on macOS ioreg. The K1 success marker
# in the IOKit-userland port plan ("ioreg -l works"). Installs to
# /usr/sbin/ioreg. run.sh runs it and emits IOKIT-IOREG-OK/FAIL.
echo "==> building ioreg(8)"
cc -fblocks \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/sbin/ioreg" \
   "$ROOT/src/libIOKit/ioreg.c" \
   -lIOKit -lCoreFoundation -lsystem_kernel -llaunch -lpthread
test -x "$WORK/rootfs/usr/sbin/ioreg" \
    || { echo "FAIL: /usr/sbin/ioreg not installed or not executable"; exit 1; }
chroot "$WORK/rootfs" ldd /usr/sbin/ioreg \
    | grep -q "libIOKit.so.* => /usr/lib/system/" \
    || { echo "FAIL: ldd doesn't resolve ioreg to /usr/lib/system/libIOKit.so"; exit 1; }
echo "==> ioreg(8) built + installed at /usr/sbin/ioreg"

# iokitnotifytest — libIOKit iter 4 notification-wiring test client.
# Validates IONotificationPort + IOServiceAddMatchingNotification
# end-to-end: allocate the port, SetDispatchQueue, register a Match
# notification, drain the initial-arming iterator, tear the port
# down. The async device-arrival callback fire path is not exercised
# in CI (QEMU hot-plug isn't injectable from the boot test); the
# underlying raw-mach_msg receive thread is structurally identical to
# HWREG-PUBSUB / SC-NOTIFY, both already CI-proven.
echo "==> building iokitnotifytest"
cc -fblocks \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/iokitnotifytest" \
   "$ROOT/src/libIOKit/iokitnotifytest.c" \
   -lIOKit -lCoreFoundation -ldispatch -lBlocksRuntime \
   -lsystem_kernel -llaunch -lpthread
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/iokitnotifytest" \
    || { echo "FAIL: iokitnotifytest not built"; exit 1; }
echo "==> iokitnotifytest built"

#
# 3s. Phase J1 iter 1 — generate libnotify MIG stubs + build libnotify.
#     Apple's libnotify client library (src/Libnotify/). Vendored at
#     Phase J0 (commit 455a727). This step:
#       (a) runs migcom on notify_ipc.defs + notify_old_ipc.defs to
#           produce notify_ipc{User,Server}.c/.h
#       (b) builds libnotify.so against those stubs + freebsd-shims
#       (c) installs to /usr/lib/system/libnotify.so + sonname link
#     notifyd daemon is J2; this is client-library-only.
#     Plan: pkgdemon.github.io/freebsd-asl-plan.html
#
echo "==> Phase J1: generating libnotify MIG stubs"
LIBNOTIFY_MIG_OUT="$WORK/libnotify-mig"
mkdir -p "$LIBNOTIFY_MIG_OUT"
LIBNOTIFY_MIG_INCS="-I$ROOT/src/libmach/include -I$ROOT/src/Libnotify"
for d in notify_ipc notify_old_ipc; do
    defs="$ROOT/src/Libnotify/${d}.defs"
    echo "  mig: ${d}.defs"
    ( cd "$LIBNOTIFY_MIG_OUT" && \
      MIGCC=/usr/bin/cc MIGCOM="$WORK/rootfs/usr/libexec/migcom" \
      /bin/sh "$ROOT/src/bootstrap_cmds/migcom.tproj/mig.sh" \
        $LIBNOTIFY_MIG_INCS \
        -header "${d}.h" -user "${d}User.c" \
        -server "${d}Server.c" -sheader "${d}Server.h" \
        "$defs" ) \
      || { echo "FAIL: mig could not process ${d}.defs"; exit 1; }
done
for f in notify_ipc.h notify_ipcUser.c; do
    test -s "$LIBNOTIFY_MIG_OUT/$f" \
        || { echo "FAIL: mig produced no $f from notify_ipc.defs"; exit 1; }
done
# notifyd.c / notify_proc.c include "notifyServer.h" / "notifyUser.h"
# but our MIG subsystem is "notify_ipc". Symlink the short names to
# the long ones — Apple's xcode build does the equivalent via build
# settings (-prefix or wrapper rules).
( cd "$LIBNOTIFY_MIG_OUT" && \
    ln -sf notify_ipc.h notifyServer.h && \
    ln -sf notify_ipc.h notifyUser.h && \
    ln -sf notify_ipcServer.c notifyServer.c && \
    ln -sf notify_ipcUser.c notifyUser.c )
echo "==> Phase J1: libnotify MIG stubs generated"
ls -la "$LIBNOTIFY_MIG_OUT"

echo "==> Phase J1: building libnotify (src/Libnotify)"
mkdir -p "$WORK/rootfs/usr/lib/system"
make -C "$ROOT/src/Libnotify" \
    DESTDIR="$WORK/rootfs" \
    SYSROOT="$WORK/rootfs" \
    MIGOUT="$LIBNOTIFY_MIG_OUT" \
    all install
ls -lh "$WORK/rootfs/usr/lib/system/libnotify.so"* 2>/dev/null
test -f "$WORK/rootfs/usr/lib/system/libnotify.so.1" \
    || { echo "FAIL: libnotify.so.1 not installed"; exit 1; }
test -L "$WORK/rootfs/usr/lib/system/libnotify.so" \
    || { echo "FAIL: libnotify.so symlink not installed"; exit 1; }
echo "==> NOTIFY-LIB-OK"

#
# 3t. Phase J1 iter 2 — generate ASL MIG stubs + build libsystem_asl.
#     Apple's libsystem_asl (src/syslog/libsystem_asl.tproj/). Plus
#     aslcommon static-lib (src/syslog/aslcommon/) merged into the .so.
#     syslogd / aslmanager / syslog(1) ship in J2-J5.
#     Plan: pkgdemon.github.io/freebsd-asl-plan.html
#
echo "==> Phase J1: generating ASL MIG stubs"
ASL_MIG_OUT="$WORK/asl-mig"
mkdir -p "$ASL_MIG_OUT"
ASL_MIG_INCS="-I$ROOT/src/libmach/include -I$ROOT/src/syslog/aslcommon -I$ROOT/src/syslog/libsystem_asl.tproj/include"
( cd "$ASL_MIG_OUT" && \
  MIGCC=/usr/bin/cc MIGCOM="$WORK/rootfs/usr/libexec/migcom" \
  /bin/sh "$ROOT/src/bootstrap_cmds/migcom.tproj/mig.sh" \
    $ASL_MIG_INCS \
    -header "asl_ipc.h" -user "asl_ipcUser.c" \
    -server "asl_ipcServer.c" -sheader "asl_ipcServer.h" \
    "$ROOT/src/syslog/aslcommon/asl_ipc.defs" ) \
  || { echo "FAIL: mig could not process asl_ipc.defs"; exit 1; }
for f in asl_ipc.h asl_ipcUser.c; do
    test -s "$ASL_MIG_OUT/$f" \
        || { echo "FAIL: mig produced no $f from asl_ipc.defs"; exit 1; }
done
echo "==> Phase J1: ASL MIG stubs generated"
ls -la "$ASL_MIG_OUT"

echo "==> Phase J1: building libsystem_asl (src/syslog/libsystem_asl.tproj)"
mkdir -p "$WORK/rootfs/usr/lib/system"
make -C "$ROOT/src/syslog/libsystem_asl.tproj" \
    DESTDIR="$WORK/rootfs" \
    SYSROOT="$WORK/rootfs" \
    MIGOUT="$ASL_MIG_OUT" \
    all install
ls -lh "$WORK/rootfs/usr/lib/system/libsystem_asl.so"* 2>/dev/null
test -f "$WORK/rootfs/usr/lib/system/libsystem_asl.so.1" \
    || { echo "FAIL: libsystem_asl.so.1 not installed"; exit 1; }
test -L "$WORK/rootfs/usr/lib/system/libsystem_asl.so" \
    || { echo "FAIL: libsystem_asl.so symlink not installed"; exit 1; }
test -f "$WORK/rootfs/usr/include/asl.h" \
    || { echo "FAIL: /usr/include/asl.h not installed"; exit 1; }
echo "==> ASL-LIB-OK"

#
# 3u. Phase J2 iter 1 — build notifyd daemon (src/Libnotify/notifyd).
#     Uses libnotify (J1) + libsystem_asl (J1) + libxpc + libdispatch.
#     MIG stubs already generated in step 3s ($WORK/libnotify-mig);
#     notifyd consumes the Server side. Install to /usr/sbin/notifyd.
#     Plan: pkgdemon.github.io/freebsd-asl-plan.html (Phase J2)
#
echo "==> Phase J2: building notifyd (src/Libnotify/notifyd)"
mkdir -p "$WORK/rootfs/usr/sbin"
make -C "$ROOT/src/Libnotify/notifyd" \
    DESTDIR="$WORK/rootfs" \
    SYSROOT="$WORK/rootfs" \
    MIGOUT="$WORK/libnotify-mig" \
    all install
ls -lh "$WORK/rootfs/usr/sbin/notifyd"
test -x "$WORK/rootfs/usr/sbin/notifyd" \
    || { echo "FAIL: /usr/sbin/notifyd not installed or not executable"; exit 1; }
echo "==> NOTIFYD-BUILD-OK"

#
# 3v. Phase J2 iter 15 — build syslogd (src/syslog/syslogd.tproj).
#     Apple syslogd daemon. Mach-only ingest first; bsd_in.c, klog_in,
#     /etc/syslog.conf parsing land in J3. Re-uses notifyd_stubs.c
#     for shared Apple-private link symbols.
#     Plan: pkgdemon.github.io/freebsd-asl-plan.html (Phase J2)
#
echo "==> Phase J2: building syslogd (src/syslog/syslogd.tproj)"
mkdir -p "$WORK/rootfs/usr/sbin"
make -C "$ROOT/src/syslog/syslogd.tproj" \
    DESTDIR="$WORK/rootfs" \
    SYSROOT="$WORK/rootfs" \
    MIGOUT="$WORK/asl-mig" \
    all install
ls -lh "$WORK/rootfs/usr/sbin/syslogd"
test -x "$WORK/rootfs/usr/sbin/syslogd" \
    || { echo "FAIL: /usr/sbin/syslogd not installed or not executable"; exit 1; }
echo "==> SYSLOGD-BUILD-OK"

#
# 3w. Phase J4 iter 1 — build aslmanager (src/syslog/aslmanager.tproj).
#     Apple ASL log rotator. Invoked on-demand by launchd via
#     com.apple.aslmanager.plist to age out /var/log/asl/* into
#     Archive/. Re-uses notifyd_stubs.c + libsystem_asl link surface.
#     Plan: pkgdemon.github.io/freebsd-asl-plan.html (Phase J4)
#
echo "==> Phase J4: building aslmanager (src/syslog/aslmanager.tproj)"
mkdir -p "$WORK/rootfs/usr/sbin"
make -C "$ROOT/src/syslog/aslmanager.tproj" \
    DESTDIR="$WORK/rootfs" \
    SYSROOT="$WORK/rootfs" \
    MIGOUT="$WORK/asl-mig" \
    all install
ls -lh "$WORK/rootfs/usr/sbin/aslmanager"
test -x "$WORK/rootfs/usr/sbin/aslmanager" \
    || { echo "FAIL: /usr/sbin/aslmanager not installed or not executable"; exit 1; }
echo "==> ASLMANAGER-BUILD-OK"

#
# 3x. Phase J5 — build syslog(1) CLI (src/syslog/util.tproj).
#     Apple's user-facing ASL tool: post messages, search the on-disk
#     store, control the daemon. FreeBSD base has no /usr/bin/syslog
#     (logger(1) is the BSD equivalent), so no conflict.
#     Plan: pkgdemon.github.io/freebsd-asl-plan.html (Phase J5)
#
echo "==> Phase J5: building syslog(1) CLI (src/syslog/util.tproj)"
mkdir -p "$WORK/rootfs/usr/bin"
make -C "$ROOT/src/syslog/util.tproj" \
    DESTDIR="$WORK/rootfs" \
    SYSROOT="$WORK/rootfs" \
    MIGOUT="$WORK/asl-mig" \
    all install
ls -lh "$WORK/rootfs/usr/bin/syslog"
test -x "$WORK/rootfs/usr/bin/syslog" \
    || { echo "FAIL: /usr/bin/syslog not installed or not executable"; exit 1; }
echo "==> SYSLOG-CLI-BUILD-OK"

#
# 3y. Phase K — build ipconfigd (src/IPConfiguration/) iter 1.
#     Apple IPConfiguration daemon port (Mach-IPC track) — the real
#     DHCPv4/DHCPv6/IPv4LL/RA client. iter 1 is the daemon skeleton:
#     getifaddrs interface enumeration + bootstrap_check_in for
#     com.apple.IPConfiguration. Later iters wire DHCPv4 + the
#     SCDynamicStore publish path that the libSystemConfiguration
#     port just built. Installs /usr/sbin/ipconfigd + the launchd
#     plist (in overlays/, copied by step 4).
#     Plan: pkgdemon.github.io/freebsd-ipconfiguration-plan.html
#     (note: that doc targets the sibling AF_UNIX repo; this Mach
#     track keeps Apple's Mach IPC + MIG, same as configd/hwregd).
#
echo "==> building ipconfigd (src/IPConfiguration)"
# Phase K iter-5a: generate the ipconfig.defs MIG stubs
# (_ipconfig_server() demux for ipconfigd, ipconfigUser.c for the
# ipconfigrpctest client). Same mig.sh + migcom path as the
# launchd / hwreg.defs MIG steps above.
IPCFG_MIG="$WORK/ipcfg-mig"
mkdir -p "$IPCFG_MIG"
( cd "$IPCFG_MIG" && \
  MIGCC=/usr/bin/cc MIGCOM="$WORK/rootfs/usr/libexec/migcom" \
  /bin/sh "$ROOT/src/bootstrap_cmds/migcom.tproj/mig.sh" \
    -I"$ROOT/src/libmach/include" \
    -I"$ROOT/src/IPConfiguration" \
    -header ipconfig.h -user ipconfigUser.c \
    -server ipconfigServer.c -sheader ipconfigServer.h \
    "$ROOT/src/IPConfiguration/ipconfig.defs" ) \
  || { echo "FAIL: mig could not process ipconfig.defs"; exit 1; }
test -s "$IPCFG_MIG/ipconfigServer.c" \
    || { echo "FAIL: mig produced no ipconfigServer.c"; exit 1; }
make -C "$ROOT/src/IPConfiguration" \
    DESTDIR="$WORK/rootfs" \
    SYSROOT="$WORK/rootfs" \
    MIGOUT="$IPCFG_MIG" \
    all install
ls -lh "$WORK/rootfs/usr/sbin/ipconfigd"
test -x "$WORK/rootfs/usr/sbin/ipconfigd" \
    || { echo "FAIL: /usr/sbin/ipconfigd not installed or not executable"; exit 1; }

# ipconfigtest — iter 1 liveness probe. bootstrap_look_up for
# com.apple.IPConfiguration; prints IPCFG-BOOT-OK on success.
# run.sh runs it and the marker gates in tests/boot-test.sh.
echo "==> building ipconfigtest"
cc -I"$ROOT/src/launchd/liblaunch" \
   -I"$ROOT/src/launchd/freebsd-shims" \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/ipconfigtest" \
   "$ROOT/src/IPConfiguration/ipconfigtest.c" \
   -llaunch -lsystem_kernel
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/ipconfigtest" \
    || { echo "FAIL: ipconfigtest not built"; exit 1; }

# ipconfigrpctest — iter 5a RPC liveness probe. Links the
# MIG-generated ipconfigUser.c (client stubs for ipconfig_if_count
# / ipconfig_if_addr). run.sh runs it after IPCFG-STORE-OK; the
# IPCFG-RPC-OK marker gates in tests/boot-test.sh.
echo "==> building ipconfigrpctest"
cc -I"$IPCFG_MIG" -I"$ROOT/src/IPConfiguration" \
   -I"$ROOT/src/launchd/liblaunch" \
   -I"$ROOT/src/launchd/freebsd-shims" \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wno-macro-redefined \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/tests/freebsd-launchd-mach/ipconfigrpctest" \
   "$ROOT/src/IPConfiguration/ipconfigrpctest.c" \
   "$IPCFG_MIG/ipconfigUser.c" \
   -llaunch -lsystem_kernel
test -x "$WORK/rootfs/usr/tests/freebsd-launchd-mach/ipconfigrpctest" \
    || { echo "FAIL: ipconfigrpctest not built"; exit 1; }

# ipconfig — iter 8 Apple-shape CLI. Same MIG-client-stub linkage as
# ipconfigrpctest; subcommand table mirrors bootp/ipconfig.tproj/
# client.c so future iters grow by appending rows. Installs at
# Apple-canonical /usr/sbin/ipconfig (no FreeBSD conflict — FreeBSD
# has no ipconfig tool; sibling /sbin/ifconfig is a different binary).
echo "==> building ipconfig (CLI)"
mkdir -p "$WORK/rootfs/usr/sbin"
cc -I"$IPCFG_MIG" -I"$ROOT/src/IPConfiguration" \
   -I"$ROOT/src/launchd/liblaunch" \
   -I"$ROOT/src/launchd/freebsd-shims" \
   -I"$WORK/rootfs/usr/include" \
   -L"$WORK/rootfs/usr/lib/system" \
   -Wno-macro-redefined \
   -Wl,-rpath,/usr/lib/system -Wl,--allow-shlib-undefined \
   -o "$WORK/rootfs/usr/sbin/ipconfig" \
   "$ROOT/src/IPConfiguration/ipconfig.c" \
   "$IPCFG_MIG/ipconfigUser.c" \
   -llaunch -lsystem_kernel
test -x "$WORK/rootfs/usr/sbin/ipconfig" \
    || { echo "FAIL: /usr/sbin/ipconfig not built"; exit 1; }
echo "==> ipconfigd + ipconfigtest + ipconfigrpctest + ipconfig built"

#
# 3z. purge build packages + clean pkg cache + tear down chroot.
#     Runs LAST in the build phase, after every chroot-side build
#     (libdispatch) has used cmake/ninja/clang. Build pkgs (cmake/ninja
#     from buildpkgs.txt; clang/lld/-dev from buildpkgs-base.txt) get
#     removed before mkuzip so they don't ship in the ISO. Pkg
#     download cache also cleared.
#
if [ -n "$BUILD_PKGS" ] || [ -n "$BASE_BUILD_PKGS" ]; then
    echo "==> purging build packages (ports + pkgbase build-only)"
    if [ -n "$BUILD_PKGS" ]; then
        # shellcheck disable=SC2086
        chroot "$WORK/rootfs" env ASSUME_ALWAYS_YES=yes \
            pkg delete -y $BUILD_PKGS
    fi
    if [ -n "$BASE_BUILD_PKGS" ]; then
        # shellcheck disable=SC2086
        chroot "$WORK/rootfs" env ASSUME_ALWAYS_YES=yes \
            pkg delete -y $BASE_BUILD_PKGS
    fi
    chroot "$WORK/rootfs" env ASSUME_ALWAYS_YES=yes \
        pkg autoremove -y || true
fi

if [ -n "$RUNTIME_PKGS" ] || [ -n "$BUILD_PKGS" ]; then
    echo "==> cleaning pkg download cache"
    chroot "$WORK/rootfs" env ASSUME_ALWAYS_YES=yes \
        pkg clean -a -y || true

    cleanup_chroot
    trap - EXIT INT TERM
fi

#
# 4. apply local overlays (etc/rc.conf, etc/motd.template,
#    /usr/tests/freebsd-launchd-mach/, ...). No "slim" rm -rf step:
#    pkg curation owns rootfs content end-to-end. If something
#    unwanted shows up, drop the pkg from pkglist-base.txt rather
#    than deleting files post-install.
#
if [ -d "$ROOT/overlays" ]; then
    echo "==> applying overlays"
    cp -aR "$ROOT/overlays/." "$WORK/rootfs/"
fi

# rc.local needs to be executable
[ -f "$WORK/rootfs/etc/rc.local" ] && chmod +x "$WORK/rootfs/etc/rc.local"

#
# 6. assemble the bootable GPT disk image (BIOS + UEFI, rw UFS root).
#    No /etc/fstab heredoc — overlays/etc/fstab carries the real root
#    entry, and overlays/boot/loader.conf.d/ carries the loader
#    settings. The kernel mounts the freebsd-ufs partition read-only;
#    launchd PID 1 remounts it read-write before starting any daemon.
#    No cd9660, no uzip, no unionfs, no ramdisk pivot.
#
CONTENT_BYTES=$(du -sk "$WORK/rootfs" | awk '{print $1*1024}')
echo "==> rootfs content = $CONTENT_BYTES bytes ($((CONTENT_BYTES / 1024 / 1024)) MiB)"

# 6a. root UFS — content plus ~1.5 GB read-write headroom. UFS label
#     "ROOTFS" matches loader.conf.d's vfs.root.mountfrom and the
#     overlays/etc/fstab entry. softupdates for crash resilience.
echo "==> makefs ffs (rw root, +1.5G headroom)"
makefs -t ffs -B little \
    -o version=2,label=ROOTFS,softupdates=1 \
    -b 1500m \
    "$WORK/rootfs.ufs" "$WORK/rootfs"
ls -lh "$WORK/rootfs.ufs"

# 6b. EFI System Partition — FAT, FreeBSD's loader.efi at the UEFI
#     fallback path /EFI/BOOT/BOOTX64.EFI. 33 MB / FAT32, matching
#     FreeBSD's own release images (a smaller ESP drops makefs to
#     FAT12, which some UEFI firmware rejects).
echo "==> building EFI System Partition"
ESPDIR="$WORK/esp-stage"
rm -rf "$ESPDIR"
mkdir -p "$ESPDIR/EFI/BOOT"
if [ -f "$WORK/rootfs/boot/loader_lua.efi" ]; then
    cp "$WORK/rootfs/boot/loader_lua.efi" "$ESPDIR/EFI/BOOT/BOOTX64.EFI"
elif [ -f "$WORK/rootfs/boot/loader.efi" ]; then
    cp "$WORK/rootfs/boot/loader.efi" "$ESPDIR/EFI/BOOT/BOOTX64.EFI"
else
    echo "ERROR: no loader.efi found in rootfs/boot/" >&2
    exit 1
fi
makefs -t msdos \
    -o fat_type=32 -o sectors_per_cluster=1 -o volume_label=EFISYS \
    -s 33292k \
    "$WORK/esp.img" "$ESPDIR"

# 6c. assemble the GPT disk image. Boots BOTH legacy BIOS (protective
#     MBR bootstrap from pmbr, chaining the freebsd-boot/gptboot
#     partition) AND UEFI (the efi/ESP partition). freebsd-ufs is the
#     read-write root.
echo "==> mkimg: GPT disk image (BIOS + UEFI)"
for f in boot/pmbr boot/gptboot; do
    [ -f "$WORK/rootfs/$f" ] || { echo "ERROR: rootfs/$f missing" >&2; exit 1; }
done
mkimg -s gpt -f raw \
    -b "$WORK/rootfs/boot/pmbr" \
    -p freebsd-boot/bootfs:="$WORK/rootfs/boot/gptboot" \
    -p efi/efiboot0:="$WORK/esp.img" \
    -p freebsd-ufs/ROOTFS:="$WORK/rootfs.ufs" \
    -o "$WORK/disk.img"
ls -lh "$WORK/disk.img"

# 6d. compress for publishing — the sparse rw headroom compresses away.
#     Ship disk.img.gz: gunzip then dd to storage, or boot directly in
#     qemu / VirtualBox / any hypervisor.
echo "==> gzip disk image"
gzip -9 -c "$WORK/disk.img" > "$OUT/disk.img.gz"
ls -lh "$OUT/disk.img.gz"
sha256 "$OUT/disk.img.gz" 2>/dev/null || sha256sum "$OUT/disk.img.gz"

# trim the multi-GB image intermediates — only out/ needs to survive
# the post-build copyback.
rm -f "$WORK/disk.img" "$WORK/rootfs.ufs" "$WORK/esp.img"

#
# 12. package mach.ko as a standalone release tarball. Users on a stock
#     FreeBSD install can fetch this, untar to /, kldload mach, without
#     building anything.
#
echo "==> packaging mach.ko standalone tarball"
MACHKO_PKG_DIR="$WORK/mach-kmod-pkg"
MACHKO_BASENAME="mach.ko-FreeBSD-${FREEBSD_VERSION}-${ARCH}"
mkdir -p "$MACHKO_PKG_DIR/boot/kernel"
cp "$WORK/rootfs/boot/kernel/mach.ko" "$MACHKO_PKG_DIR/boot/kernel/mach.ko"
cat > "$MACHKO_PKG_DIR/README" <<EOF
mach.ko — out-of-tree FreeBSD Mach IPC kernel module.

Built against: FreeBSD ${FREEBSD_VERSION} (${ARCH})
Built on:      $(date -u +%Y-%m-%dT%H:%M:%SZ)

Install:
  tar -xJf ${MACHKO_BASENAME}.tar.gz -C /
  echo 'mach_load="YES"' >> /boot/loader.conf
  # reboot, or kldload mach
EOF
( cd "$MACHKO_PKG_DIR" && tar -czf "$OUT/${MACHKO_BASENAME}.tar.gz" boot README )
ls -lh "$OUT/${MACHKO_BASENAME}.tar.gz"
sha256 "$OUT/${MACHKO_BASENAME}.tar.gz" 2>/dev/null || sha256sum "$OUT/${MACHKO_BASENAME}.tar.gz"

echo
echo "==> disk image:    $(ls -lh "$OUT/disk.img.gz" | awk '{print $5}')  (disk.img.gz, gzip-compressed)"
echo "==> mach.ko tarball: $(ls -lh "$OUT/${MACHKO_BASENAME}.tar.gz" | awk '{print $5}')"
echo "==> DONE"
