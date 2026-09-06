#!/bin/sh
# build.sh — assemble bootable NextBSD images by INSTALLING the packages.
#
# The entire OS now comes from the nextbsd-pkg flat repo: one `pkg install
# NextBSD-everything` lays down the freebsd-compat base + kernel + Darwin
# userland (incl. the LaunchDaemons, bundled into the NextBSD-userland package)
# + kexts. The user-editable /etc config (accounts, sshd_config, pam.d, fstab,
# ...) is NOT package-owned — it is seeded from the nextbsd-overlays repo below
# so `pkg upgrade` can never clobber it. There is NO in-chroot compile — src/ is
# gone — and no tar-extract/hand-drop of base/kernel/kexts.
#
# Runs inside a FreeBSD 15.1 VM (vmactions) — a REAL FreeBSD host, so pkg(8) is
# native and installs FreeBSD:15 packages with no ABI/OSVERSION hackery. The
# per-arch lanes (amd64, aarch64) run as independent parallel jobs.
#
# Everything is host-driven (pkg -r, pwd_mkdb -d, cap_mkdb <target>) rather than
# chroot'd, so the aarch64 image assembles in the same x86 VM without qemu-user
# (the NextBSD-* packages ship no exec'ing post-install scripts; makefs/mkuzip/
# mkimg are arch-agnostic).
#
# Produces:
#   out/NextBSD-<arch>-<date>.img.zip   GPT disk image (BIOS+UEFI, rw UFS root)
#   out/NextBSD-<arch>-<date>.iso.zip   live ISO (mfsroot -> uzip -> vfs.pivot)
#
# BOARD=rpi500 instead produces one artifact and no ISO:
#   out/NextBSD-rpi500-<arch>-<date>.img.zip   MBR: FAT32 boot + rw UFS root
# The board token comes FIRST and that is load-bearing -- build.yml's release
# step does IMG=$(ls out/NextBSD-${ARCH}-*.img.zip) into a scalar, so an
# arch-first name would match two files and break the existing arm64 lane.

set -eu

: "${FREEBSD_VERSION:=15.1}"
: "${LABEL:=NEXTBSD}"
ARCH_EXPLICIT=${ARCH:+yes}
ARCH=${ARCH:-amd64}
# UTC build timestamp — the SINGLE source of truth for the version: CI passes
# IMG_DATE so the artifact name, image, nextbsd-version and os-release all match.
: "${IMG_DATE:=$(date -u +%Y%m%d-%H%M%S)}"

# Two names for 64-bit ARM, and they are NOT interchangeable:
#   ARCH=arm64      FreeBSD MACHINE. The download mirror path, the src tree's
#                   release/<arch>/ scripts, the nextbsd-pkg release tag
#                   (continuous-<arch>) and our artifact names all use this.
#   ABIARCH=aarch64 MACHINE_ARCH — what pkg(8) puts in the ABI string.
# Normalize an `aarch64` ARCH to `arm64` so a hand-run `ARCH=aarch64 sh build.sh`
# still finds the mirror, the pkg repo tag and mkisoimages.sh.
case "$ARCH" in arm64|aarch64) ARCH=arm64; ABIARCH=aarch64 ;; *) ABIARCH="$ARCH" ;; esac
PKG_ABI="FreeBSD:15:${ABIARCH}"

# BOARD selects a board-specific media lane. Empty (the default) is the
# generic PC/UEFI lane that has always been here. The rootfs is identical
# either way -- the same packages, the same makefs -- and only the boot half
# and the partitioning differ, which is the whole reason this is one script.
BOARD=${BOARD:-}
case "$BOARD" in
"") ;;
rpi500)
    # The Pi 500+ is arm64 and only arm64. Catch a contradictory ARCH here
    # rather than 40 minutes later, when an amd64 rootfs meets an aarch64
    # kernel8.img and the board boots to silence.
    if [ "${ARCH}" != arm64 ] && [ -n "${ARCH_EXPLICIT:-}" ]; then
        echo "ERROR: BOARD=rpi500 is arm64; got ARCH=$ARCH." >&2
        exit 1
    fi
    ARCH=arm64; ABIARCH=aarch64; PKG_ABI="FreeBSD:15:aarch64"
    ;;
*)
    echo "ERROR: unknown BOARD=$BOARD (known: rpi500)." >&2
    exit 1
    ;;
esac

ROOT=$(cd "$(dirname "$0")" && pwd)
WORK=$ROOT/work
OUT=$ROOT/out
DIST=$ROOT/distfiles
RF=$WORK/rootfs
MIRROR="https://download.freebsd.org/ftp/releases/${ARCH}/${FREEBSD_VERSION}-RELEASE"
# nextbsd-pkg flat repo: per-arch rolling release tag (continuous-<arch>).
PKG_REPO_URL="https://github.com/nextbsd-redux/nextbsd-pkg/releases/download/continuous-${ARCH}"

mkdir -p "$WORK" "$OUT" "$DIST"
# A prior run's rootfs carries schg (system-immutable) flags; clear before rm.
chflags -R noschg "$WORK" 2>/dev/null || true
rm -rf "$WORK"/* "$OUT"/*

echo "==> build: NextBSD $ARCH${BOARD:+ board=$BOARD} (pkg ABI $PKG_ABI), stamp $IMG_DATE"

# ---------------------------------------------------------------------------
# 1. src.txz — ONLY for FreeBSD's release script (mkisoimages.sh) in the ISO
#    tail. Nothing is compiled from it; it is not laid into the rootfs.
# ---------------------------------------------------------------------------
if [ ! -f "$DIST/src.txz" ]; then
    echo "==> downloading src.txz"
    fetch -o "$DIST/src.txz" "$MIRROR/src.txz"
fi
echo "==> extracting src.txz for mkisoimages.sh"
mkdir -p "$WORK/freebsd-src"
tar -xJf "$DIST/src.txz" -C "$WORK/freebsd-src"

# ---------------------------------------------------------------------------
# 2. Install the whole OS from packages into the rootfs.
# ---------------------------------------------------------------------------
mkdir -p "$RF"

# BOOTSTRAP pkg TLS: rehash the build VM's CA store so pkg can fetch the NextBSD
# flat repo over https (GitHub release assets). certctl ships in the base VM.
certctl rehash 2>/dev/null || true

# ISOLATE to ONLY the NextBSD repo. The vmactions VM ships enabled FreeBSD
# pkgbase/ports repos that are SIGNED with keys we don't have (they error
# "Error loading trusted certificates" and jam the SAT solver). Point pkg at a
# private REPOS_DIR that contains just the unsigned NextBSD flat repo.
NBREPO="$WORK/nbrepo"; mkdir -p "$NBREPO"
cat > "$NBREPO/NextBSD.conf" <<CONF
NextBSD: {
  url: "${PKG_REPO_URL}",
  enabled: yes,
  signature_type: none,
}
CONF

# Install into $RF. `-r` sets the install root (host-driven, not chroot).
#   ABI=FreeBSD:15:<arch>  — for arm64 we cross-install aarch64 packages in the
#                            x86 VM; amd64 matches the VM natively.
#   OSVERSION=$(uname -K)  — pkg requires OSVERSION when ABI is set; the VM
#                            kernel (15.1) is the right value for both arches.
#   IGNORE_OSVERSION       — the rolling snapshot's stamp shouldn't gate install.
export ASSUME_ALWAYS_YES=yes IGNORE_OSVERSION=yes
export ABI="$PKG_ABI" OSVERSION="$(uname -K)"
PKG="pkg -r $RF -o REPOS_DIR=$NBREPO"
echo "==> pkg update + install NextBSD-everything into $RF (ABI=$ABI OSVERSION=$OSVERSION)"
$PKG update -f
$PKG install -y NextBSD-everything
# Fail loudly if the install laid down nothing (empty rootfs -> later steps die
# with confusing errors). The user /etc config is seeded from nextbsd-overlays
# below (deliberately NOT package-owned), so guard on a package-core binary.
[ -x "$RF/sbin/launchd" ] || { echo "ERROR: NextBSD-everything install produced no /sbin/launchd" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 3. Apple /private layout + runtime skeleton.
#    The package ships /private/etc (master.passwd, plists, ...) but NOT the
#    /etc,/var,/tmp -> private symlinks, and not the mutable /var runtime.
# ---------------------------------------------------------------------------
mkdir -p "$RF/private"
for _pd in etc var tmp; do
    if [ -d "$RF/$_pd" ] && [ ! -L "$RF/$_pd" ]; then
        mv "$RF/$_pd" "$RF/private/$_pd"
    else
        mkdir -p "$RF/private/$_pd"
    fi
    ln -s "private/$_pd" "$RF/$_pd"
done

# 3b. Seed the pkg-free, admin-owned /etc + loader fragment from nextbsd-overlays.
#     These are laid down by the assembler, NOT any package, so `pkg upgrade`
#     never clobbers a user's accounts / SSH / PAM config. Cloned fresh each build
#     so a new image always carries the current defaults. (They were split out of
#     the NextBSD-userland package's overlay/ for exactly this reason.)
OVL="$WORK/nextbsd-overlays"
rm -rf "$OVL"
git clone --depth 1 https://github.com/nextbsd-redux/nextbsd-overlays "$OVL"
cp -R "$OVL/rootfs/." "$RF/"
chmod 0600 "$RF/private/etc/master.passwd"
[ -s "$RF/private/etc/master.passwd" ] || { echo "ERROR: nextbsd-overlays seed produced no master.passwd" >&2; exit 1; }

# launchd -w job-overrides DB dir so launchd loads cleanly at boot.
mkdir -p "$RF/private/var/db/launchd.db/com.apple.launchd"
# /var skeleton + utmpx (PAM pam_open_session needs utx.* or login aborts).
mkdir -p "$RF/var/run" "$RF/var/log" "$RF/var/db" "$RF/var/empty" \
         "$RF/var/tmp" "$RF/tmp" "$RF/dev"
chmod 1777 "$RF/tmp" "$RF/var/tmp"
# /usr/share/locale must exist or locale(1) err()s on opendir before it can
# even list the built-in C/POSIX locales (init_locales_list() aborts). The
# curated base ships no locale data; create the dir so the C locale works.
mkdir -p "$RF/usr/share/locale"
: > "$RF/var/run/utx.active"; : > "$RF/var/log/utx.lastlogin"; : > "$RF/var/log/utx.log"
chmod 644 "$RF/var/run/utx.active" "$RF/var/log/utx.lastlogin" "$RF/var/log/utx.log"
mkdir -p "$RF/root"; chmod 0700 "$RF/root"

# ---------------------------------------------------------------------------
# 4. Regenerate /etc databases from the package's master.passwd/login.conf.
#    HOST-DRIVEN (no chroot) so this runs for aarch64 in the x86 VM too:
#    pwd_mkdb/cap_mkdb output is arch-neutral (both targets little-endian).
# ---------------------------------------------------------------------------
chown -RH 0:0 "$RF/etc" 2>/dev/null || true
pwd_mkdb -p -d "$RF/etc" "$RF/etc/master.passwd"
[ -f "$RF/etc/login.conf" ] && cap_mkdb "$RF/etc/login.conf"
[ -f "$RF/usr/share/misc/termcap" ] && cap_mkdb "$RF/usr/share/misc/termcap" || true
# CA trust store for the SHIPPED image (best-effort; the package ships cert.pem).
env DESTDIR="$RF" certctl rehash 2>/dev/null || true

# ---------------------------------------------------------------------------
# 5. pkg repos in the SHIPPED image (arch-dynamic) so a user on the installed
#    system can `pkg install` out of the box. Exactly TWO repos, deliberately:
#      - FreeBSD: pkg.FreeBSD.org/latest  (FreeBSD *packages* — ports)
#      - NextBSD: the nextbsd-pkg continuous flat repo (the NextBSD base/world)
#    NOT the FreeBSD pkgbase base/kmods repos — NextBSD supplies its own base,
#    and we never want FreeBSD-base/FreeBSD-kernel packages on the image. Those
#    are only ever on the build VM (in its own /etc/pkg) and are never written
#    into $RF. NextBSD rebrands the ABI, so pin FreeBSD:15:<arch> + ignore the
#    osversion gate (#357). The NextBSD flat repo is unsigned (GitHub assets).
# ---------------------------------------------------------------------------
mkdir -p "$RF/usr/local/etc/pkg/repos"
cat > "$RF/usr/local/etc/pkg/repos/FreeBSD.conf" <<CONF
FreeBSD: {
  url: "pkg+https://pkg.FreeBSD.org/FreeBSD:15:${ABIARCH}/latest",
  mirror_type: "srv",
  enabled: yes
}
CONF
cat > "$RF/usr/local/etc/pkg/repos/NextBSD.conf" <<CONF
NextBSD: {
  url: "${PKG_REPO_URL}",
  enabled: yes,
  signature_type: none,
}
CONF
cat > "$RF/usr/local/etc/pkg.conf" <<CONF
ABI = "FreeBSD:15:${ABIARCH}";
IGNORE_OSVERSION = yes;
CONF

# ---------------------------------------------------------------------------
# 5b. Bake pkglist.txt packages into the shipped rootfs from the FreeBSD ports
#     repo, so BOTH the published .img and .iso (same rootfs) ship them out of
#     the box (pkg + git). Uses the FreeBSD.conf written just above via a
#     REPOS_DIR override so pkg ignores the build VM's own signed repos (which
#     jam the SAT solver). pkg resolves each package's full dependency closure
#     automatically — no manual runtime-lib audit (unlike the base srclist); the
#     libscan step below then re-verifies the closure over the whole rootfs.
#     ABI / OSVERSION / IGNORE_OSVERSION / ASSUME_ALWAYS_YES are still exported
#     from the NextBSD-everything install above.
# ---------------------------------------------------------------------------
if [ -f "$ROOT/pkglist.txt" ]; then
    EXTRA_PKGS=$(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$ROOT/pkglist.txt" | tr '\n' ' ')
    if [ -n "$EXTRA_PKGS" ]; then
        echo "==> installing pkglist.txt packages from FreeBSD repo: $EXTRA_PKGS"
        FBSD_REPOS="$RF/usr/local/etc/pkg/repos"
        pkg -r "$RF" -o REPOS_DIR="$FBSD_REPOS" update -f
        pkg -r "$RF" -o REPOS_DIR="$FBSD_REPOS" install -y $EXTRA_PKGS
    fi
fi

# ---------------------------------------------------------------------------
# 5b. Drop pkg's build residue before makefs bakes the tree in. Two separate
#     things accumulate in the rootfs, and neither is needed at runtime:
#
#       - the downloaded package tarballs, in PKG_CACHEDIR (/var/cache/pkg).
#         Every install above (NextBSD-everything, then pkglist.txt) leaves its
#         .pkg files there. `pkg clean -a` is scoped to exactly this.
#       - the fetched repository catalogues, in PKG_DBDIR
#         (/var/db/pkg/repos/<repo>/db), written by the `pkg update` calls.
#         `pkg clean` does NOT touch these and there is no pkg subcommand that
#         does, so they have to go by hand.
#
#     Both of those are the pkg-visible paths; on disk they live under
#     private/var, since step 3 made /var a relative symlink -> private/var.
#     `pkg -r` walks that symlink (only a *trailing* symlink wouldn't be
#     followed), so the clean below resolves to private/var/cache/pkg; the rm
#     names private/ explicitly, as the rest of this script does.
#
#     Both regenerate on the installed system: pkg redownloads a tarball when
#     asked to install, and refetches a catalogue on any install/fetch/search
#     (REPO_AUTOUPDATE defaults to YES) or an explicit `pkg update`.
#     The catalogues are not small — on gershwin-on-freebsd the upstream
#     `latest` catalogue grew 71 MiB -> 1.66 GiB, shipped twice, which added
#     3.3 GiB to the staged tree and ~446 MiB to the compressed image, and the
#     bloated /var then OOM'd that project's boot test.
#     Runs before ALL THREE consumers: the rw disk image (step 6), the compact
#     rootfs.uzip (step 7), and the mfsroot.
#     KEEP: /usr/local/etc/pkg/repos — the repo *config* written in step 5, which
#     the shipped system needs so `pkg install` works out of the box — and
#     private/var/db/pkg/local.sqlite, the installed-package registry.
#     Host-driven via `pkg -r`, matching the rest of this script (no chroot).
# ---------------------------------------------------------------------------
echo "==> pkg clean: drop cached tarballs + fetched repository catalogues"
pkg -r "$RF" clean -a -y || true
rm -rf "$RF/private/var/db/pkg/repos"

# ---------------------------------------------------------------------------
# 6. Version identity (single source of truth = $IMG_DATE).
# ---------------------------------------------------------------------------
cat > "$RF/etc/os-release" <<OSREL
NAME="NextBSD"
PRETTY_NAME="NextBSD ${IMG_DATE}"
ID=nextbsd
ID_LIKE=freebsd
VERSION="${IMG_DATE}"
VERSION_ID="15.1"
HOME_URL="https://nextbsd.org"
OSREL
# /bin/nextbsd-version (if the userland package didn't ship one, write a stub).
if [ ! -x "$RF/bin/nextbsd-version" ]; then
    cat > "$RF/bin/nextbsd-version" <<NBV
#!/bin/sh
echo "NextBSD ${IMG_DATE}"
NBV
    chmod 0555 "$RF/bin/nextbsd-version"
fi

# Retire the kld* user CLIs (#193, Apple-shape): macOS ships kextload/kextstat,
# not kldload — only the CLI front-ends go; the kld*(2) syscalls stay, and
# kext_tools (userland) provides the kext* replacements. The curated base ships
# only kldxref (rm the whole set anyway; harmless if a name is absent). The
# kernel has NO_MODULES, so no linker.hints to generate.
rm -f "$RF/sbin/kldload" "$RF/sbin/kldunload" "$RF/sbin/kldstat" \
      "$RF/sbin/kldconfig" "$RF/usr/sbin/kldxref" \
      "$RF/usr/lib/debug/usr/sbin/kldxref.debug"

# Bake root:wheel ownership across the tree before the media tail (which also
# re-chowns before makefs, but do it here so the pkg-installed perms are sane).
chown -R 0:0 "$RF" 2>/dev/null || true

#
# 6. assemble the bootable GPT disk image (BIOS + UEFI, rw UFS root).
#    No /etc/fstab heredoc — the nextbsd-overlays seed (rootfs/private/etc/fstab) carries the real root
#    entry, and nextbsd-overlays rootfs/boot/loader.conf.d/ carries the loader
#    settings. The kernel mounts the freebsd-ufs partition read-only;
#    launchd PID 1 remounts it read-write before starting any daemon.
#    No cd9660, no uzip, no unionfs, no ramdisk pivot.
#
CONTENT_BYTES=$(du -sk "$WORK/rootfs" | awk '{print $1*1024}')
echo "==> rootfs content = $CONTENT_BYTES bytes ($((CONTENT_BYTES / 1024 / 1024)) MiB)"

# 5z. ELF shared-library closure check. overlay-first builds the base from
#     a curated srclist; any tool whose NEEDED soname isn't provided in the
#     rootfs fails at ld-elf.so.1 load time (e.g. /sbin/mount needing the
#     silently-pkgbase-provided libxo.so.0 -> launchd's rw remount exited 1
#     -> read-only root). Scan every ELF's NEEDED entries against the .so
#     files actually present in the rootfs and report any with no provider.
#     Diagnostic only (never fatal): surfaces the COMPLETE missing-lib set
#     in one build instead of one boot-failure round-trip per missing lib.
echo "==> [libscan] ELF shared-library closure over rootfs"
( set +e
  RF="$WORK/rootfs"
  find "$RF" \( -name '*.so' -o -name '*.so.*' \) \( -type f -o -type l \) \
      -exec basename {} \; 2>/dev/null | sort -u > /tmp/provided.txt
  : > /tmp/needed.txt
  find "$RF/bin" "$RF/sbin" "$RF/usr/bin" "$RF/usr/sbin" "$RF/libexec" \
       "$RF/lib" "$RF/usr/lib" "$RF/usr/libexec" -type f 2>/dev/null | while IFS= read -r f; do
    readelf -d "$f" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\].*/\1/p'
  done | sort -u > /tmp/needed.txt
  echo "    provided .so files: $(wc -l < /tmp/provided.txt), distinct NEEDED sonames: $(wc -l < /tmp/needed.txt)"
  miss=0
  while IFS= read -r so; do
    [ -z "$so" ] && continue
    grep -qx "$so" /tmp/provided.txt || { echo "    MISSING: $so"; miss=$((miss+1)); }
  done < /tmp/needed.txt
  echo "    total NEEDED sonames with no provider in rootfs: $miss"
)
echo "==> [libscan] end"

# 6a. root UFS — content plus ~1.5 GB read-write headroom. UFS label
#     "ROOTFS" matches loader.conf.d's vfs.root.mountfrom and the
#     the nextbsd-overlays seed (rootfs/private/etc/fstab) entry. softupdates for crash resilience.
# Force the ENTIRE staged tree to root:wheel (uid/gid 0) before makefs records
# it. makefs bakes in the staging tree's on-disk ownership, but the tree is
# assembled by the unprivileged build user: mkdir'd top-level dirs (/, /usr,
# /boot, /private) take the builder's gid via BSD parent-group inheritance, and
# anything cp'd/extracted from build-user-owned sources keeps its 1001:1001
# ownership. Both leaked into shipped images (e.g. / as root:1001, /usr /boot
# /private as 1001:1001). A blanket recursive chown fixes them all. We run as
# root, so setuid/setgid bits on already-0:0 files are preserved. This also
# subsumes the old narrow chown of / + System/Library/Extensions (OSKext's
# cache-dir walk requires / owned by root, else "Can't create kext cache
# under / - owner not root").
chown -R 0:0 "$WORK/rootfs"
echo "==> makefs ffs (rw root, +1.5G headroom)"
makefs -t ffs -B little \
    -o version=2,label=ROOTFS,softupdates=1 \
    -b 1500m \
    "$WORK/rootfs.ufs" "$WORK/rootfs"
ls -lh "$WORK/rootfs.ufs"

# ---------------------------------------------------------------------------
# 6b-rpi. Raspberry Pi 500+ boot partition + MBR image   (BOARD=rpi500)
#
#   The BCM2712 bootloader lives in EEPROM and boots what the plan calls route
#   (a): it reads config.txt off the first FAT partition, loads the DTB named
#   there, patches it (memory size, MAC, the RP1 window), loads kernel8.img and
#   enters it at EL2 with x0 = the FDT physical address. No loader(8), no UEFI.
#   That is why this lane produces an .img and no .iso: the firmware looks for
#   a FAT partition holding config.txt, and an ISO 9660 image has neither, so
#   it is never even opened. Booting NextBSD from optical media on this board
#   is not a missing feature, it is not a thing the hardware does.
#
#   It also means nothing can be kldload'ed at boot -- there is no loader to do
#   it -- so every boot-critical driver is compiled into the board kernel.
#
#   None of bootcode.bin / start*.elf / fixup*.dat belong here. Those are the
#   pre-2712 VideoCore boot chain; Raspberry Pi OS ships them only because one
#   boot partition serves every model back to the Pi 2. Measured on the board:
#   a 2712 boot partition needs exactly config.txt, a DTB, and the kernel.
#
#   The root half needs nothing from this partition at all. The board kernel
#   carries ROOTDEVNAME="ufs:/dev/ufs/ROOTFS" and INIT_PATH=/sbin/launchd
#   compiled in (nextbsd-kernel#11 / #180), so it mounts root by GEOM label and
#   execs launchd with no tunables passed from anywhere. The MBR slice layout
#   below is therefore not load-bearing for finding root.
# ---------------------------------------------------------------------------
if [ "$BOARD" = rpi500 ]; then
    echo "==> rpi500: staging the FAT boot partition"
    BOOTSTAGE="$WORK/rpi-boot"
    rm -rf "$BOOTSTAGE"
    mkdir -p "$BOOTSTAGE"

    # kernel8.img is kernel.bin -- the kernel wrapped in an arm64 Linux Image
    # header -- and NOT /boot/kernel/kernel, which is an ELF the firmware will
    # not enter. Default to a package-provided one so a shipping board kernel
    # (nextbsd-kernel#85) drops straight in; KERNEL8 overrides it for a PR
    # build that pulls nextbsd-kernel's rpi500-kernel8-arm64 artifact instead.
    KERNEL8=${KERNEL8:-$RF/boot/kernel8.img}
    if [ ! -f "$KERNEL8" ]; then
        echo "ERROR: no kernel8.img to put on the boot partition." >&2
        echo "       Either set KERNEL8=/path/to/kernel.bin, or install a board" >&2
        echo "       kernel package that ships /boot/kernel8.img." >&2
        exit 1
    fi
    cp "$KERNEL8" "$BOOTSTAGE/kernel8.img"

    # Assert the Image header before shipping it. The firmware validates
    # nothing: hand it an ELF and it jumps into the ELF header, and the board
    # goes black with no console and no message. Everything fails as silence on
    # this hardware, so the checks that can happen here, happen here.
    # magic 0x644d5241 at offset 56, little-endian -> bytes 41 52 4d 64.
    MAGIC=$(dd if="$BOOTSTAGE/kernel8.img" bs=1 skip=56 count=4 2>/dev/null |
        od -An -tx1 | tr -d ' \n')
    if [ "$MAGIC" != "41524d64" ]; then
        echo "ERROR: $KERNEL8 carries no arm64 Image header." >&2
        echo "       magic at offset 56 = ${MAGIC:-<empty>}, expected 41524d64." >&2
        echo "       This looks like /boot/kernel/kernel (ELF) rather than kernel.bin." >&2
        exit 1
    fi
    echo "    kernel8.img $(ls -l "$BOOTSTAGE/kernel8.img" | awk '{print $5}') bytes, Image header ok"

    # The device tree must be Raspberry Pi's own. The firmware reads, patches
    # and hands over THIS blob, and its patching only understands the vendor
    # layout -- a DTB we compiled ourselves would arrive without the memory
    # node, the MAC, or the RP1 window fixed up. Measured on the board:
    # /proc/device-tree/compatible reads "raspberrypi,500 brcm,bcm2712" and the
    # firmware selects bcm2712-rpi-500.dtb, which is the 500+ too.
    : "${RPI_FIRMWARE_TAG:=1.20250430}"

    # Ship EVERY BCM2712 device tree and let the firmware pick.
    #
    # The firmware selects a DTB by board revision code, which is the mechanism
    # designed for exactly this -- "This selection is automatic, and allows the
    # same SD card image to be used in a variety of devices". Pinning
    # device_tree= to one file is what made this image Pi 500/500+ only: any
    # other board would have been handed a device tree for hardware it is not.
    #
    # Note there are three separate Pi 5 B blobs, because the firmware
    # distinguishes silicon steppings. That is also the reason this is not a
    # guarantee for untested boards -- see the coverage note below.
    RPI_DTBS="bcm2712-rpi-5-b bcm2712d0-rpi-5-b bcm2712-d-rpi-5-b \
              bcm2712-rpi-500 \
              bcm2712-rpi-cm5-cm4io bcm2712-rpi-cm5-cm5io \
              bcm2712-rpi-cm5l-cm4io bcm2712-rpi-cm5l-cm5io"
    if [ -n "${DTB:-}" ]; then
        echo "==> rpi500: using DTB override $DTB"
        cp "$DTB" "$BOOTSTAGE/$(basename "$DTB")"
    else
        for d in $RPI_DTBS; do
            _dtb="$DIST/${RPI_FIRMWARE_TAG}-${d}.dtb"
            if [ ! -f "$_dtb" ]; then
                echo "==> fetching ${d}.dtb from raspberrypi/firmware $RPI_FIRMWARE_TAG"
                fetch -o "$_dtb" \
                    "https://raw.githubusercontent.com/raspberrypi/firmware/${RPI_FIRMWARE_TAG}/boot/${d}.dtb"
            fi
            cp "$_dtb" "$BOOTSTAGE/${d}.dtb"
            # A flattened device tree opens with magic 0xd00dfeed, big-endian.
            MAGIC=$(dd if="$BOOTSTAGE/${d}.dtb" bs=1 count=4 2>/dev/null | od -An -tx1 | tr -d ' \n')
            if [ "$MAGIC" != "d00dfeed" ]; then
                echo "ERROR: ${d}.dtb is not a flattened device tree (magic=$MAGIC)." >&2
                exit 1
            fi
        done
        _lic="$DIST/${RPI_FIRMWARE_TAG}-LICENCE.broadcom"
        [ -f "$_lic" ] || fetch -o "$_lic" \
            "https://raw.githubusercontent.com/raspberrypi/firmware/${RPI_FIRMWARE_TAG}/boot/LICENCE.broadcom"
        cp "$_lic" "$BOOTSTAGE/LICENCE.broadcom"
    fi
    echo "    $(ls "$BOOTSTAGE"/*.dtb | wc -l | tr -d ' ') device trees, FDT magic ok"

    # Device-tree overlays (nextbsd#429).
    #
    # These have to be on the FAT32 firmware partition, which no package can
    # write: pkg(8) installs into the UFS root, and this partition is neither
    # mounted at install time nor in any package's file list. The image build
    # is the only thing that writes here, so it stages them.
    #
    # They come from nextbsd-kernel-extensions' continuous release, where they
    # are published alongside the kexts, so an overlay stays versioned with the
    # driver it enables -- the two must agree about the node's compatible
    # string.
    #
    # Missing overlays are NOT fatal: the image still boots, the driver simply
    # cannot bind. That is the pre-#429 behaviour, so a fetch failure degrades
    # to it rather than breaking the build.
    _ovl="$DIST/rpi5-overlays.tar.gz"
    if [ ! -f "$_ovl" ]; then
        echo "==> fetching rpi5-overlays.tar.gz from nextbsd-kernel-extensions"
        fetch -o "$_ovl" \
            "https://github.com/nextbsd/nextbsd-kernel-extensions/releases/download/continuous/rpi5-overlays.tar.gz" \
            || echo "WARNING: no overlays asset; KMS drivers will not bind (nextbsd#429)" >&2
    fi
    if [ -s "$_ovl" ]; then
        mkdir -p "$BOOTSTAGE/overlays"
        tar -C "$BOOTSTAGE/overlays" -xzf "$_ovl"
        # The .dts sources ship in the same directory; the firmware only reads
        # .dtbo, so drop them rather than putting sources on a boot partition.
        rm -f "$BOOTSTAGE/overlays"/*.dts
        for _o in "$BOOTSTAGE/overlays"/*.dtbo; do
            [ -e "$_o" ] || continue
            MAGIC=$(dd if="$_o" bs=1 count=4 2>/dev/null | od -An -tx1 | tr -d ' \n')
            if [ "$MAGIC" != "d00dfeed" ]; then
                echo "ERROR: $(basename "$_o") is not a flattened device tree (magic=$MAGIC)." >&2
                exit 1
            fi
        done
        echo "    $(ls "$BOOTSTAGE/overlays"/*.dtbo 2>/dev/null | wc -l | tr -d ' ') overlays staged, FDT magic ok"
    fi

    cat > "$BOOTSTAGE/config.txt" <<CFG
# NextBSD on the Raspberry Pi 500+ (BCM2712).
#
# The EEPROM bootloader reads this file, loads device_tree=, patches it, loads
# kernel= and enters it at EL2 with x0 = the FDT physical address. There is no
# loader(8) anywhere in that path, so anything the kernel needs in order to
# boot is compiled into it rather than loaded here.
kernel=kernel8.img
cmdline=cmdline.txt
arm_64bit=1

# Deliberately NO device_tree= line. The firmware selects the right blob for
# the board it is running on, from the ones shipped alongside this file --
# which is what lets one image serve the whole Pi 5 family. Pinning it here
# would hand every board but one a device tree for hardware it is not.

# The kernel's console is UART10 inside the BCM2712 at 0x10_7d00_1000 -- the
# 3-pin JST-SH debug header on the board, not the 40-pin GPIO header. Which
# UART that is gets decided by a compiled-in hw.uart.console, not by this file.
# enable_uart=1 is still required: it pins the VPU core clock, and without it
# the divisor moves with clock scaling and the console degrades to line noise
# partway through boot.
enable_uart=1

# Video. Without these the firmware sets up no display and allocates no
# framebuffer, vt(4) comes up as "init without driver", and the machine boots
# to a black screen -- which on this board is indistinguishable from a kernel
# that never started.
#
# max_framebuffers is what makes the firmware allocate one at all.
# display_auto_detect lets it probe and configure whatever is attached.
#
# hdmi_force_hotplug is the interesting one, and it is here deliberately.
# Measured on a Pi 500+: the firmware reported "hotplug 0" with a monitor
# attached and its EDID perfectly readable -- DDC works (pins 15/16), hotplug
# detect (pin 19) does not, which is what a micro-HDMI adapter that skips that
# pin looks like. Raspberry Pi OS has the same workaround in its own
# cmdline.txt as the "D" in video=HDMI-A-1:1280x720@60D, so the fault is
# common enough that their default image carries a fix for it.
#
# The cost is a few MB of framebuffer allocated on a headless machine. The
# alternative failure is a black screen with no message, on hardware whose
# only other console is a cable inside the case. Worth the trade here; it is
# also the first thing to remove if a display misbehaves.
max_framebuffers=2
display_auto_detect=1
hdmi_force_hotplug=1

# framebuffer_depth is deliberately NOT set here any more (nextbsd#429 follow-up).
#
# It was here because the console runs on the firmware framebuffer: left unset
# the firmware allocates 16bpp, bcm2835_fbd(4) asks it for 24, and the console
# comes up with a blue cast and text so dim it is nearly unreadable:
#
#   changing fb bpp from 16 to 24
#   fb0: 1920x1080(1920x1080@0,0) 24bpp     <- wrong, unreadable
#   keeping existing fb bpp of 32
#   fb0: 1920x1080(1920x1080@0,0) 32bpp     <- correct
#
# 24bpp is three bytes per pixel with no padding; a driver writing 32-bit
# pixels into it lands a byte out of alignment on every pixel, which smears
# the colour channels.
#
# The intent is that KMS owns the display instead, now that VideoCoreKMS
# autoloads (nextbsd-kernel-extensions#185) -- at which point the firmware's
# choice of depth stops mattering because nothing is drawing into its
# framebuffer.
#
# UNVERIFIED ON HARDWARE. The Pi was unreachable when this was written, so the
# console has not been looked at without this line. The evidence that it may
# still be needed:
#
#   - the firmware allocates the console framebuffer before any kext can load,
#     so autoload does not change what happens at boot
#   - KMS does not take over the console. Measured with KMS loaded and
#     /dev/dri/card0 present, the console was still on the firmware fb:
#         fb0: <BCM2835 VT framebuffer driver> on simplebus0
#         VT: initialize with new VT driver "fb"      <- "fb", not "drmfb"
#     because no fbdev emulation is wired up (drm_fbdev_dma_setup is a stub).
#   - nextbsd#425, "scfb segfaults on a 16bpp framebuffer (works at 32bpp)",
#     is open.
#
# If the console comes up dim, blue-cast or blank on a Pi 5 after this change,
# this line is why, and restoring it is the fix. The real precondition is fbdev
# emulation so vt(4) moves onto drmfb.

# Deliberately NO hardcoded resolution. Forcing hdmi_group/hdmi_mode or
# framebuffer_width/height WAS measured to be ignored -- the firmware read
# EDID and chose 1920x1080 over the 720p asked for, which is the better
# answer and the one that works on someone else's monitor. Note that depth
# above is the exception: do not generalise "the framebuffer_* settings are
# ignored" from width and height, which is a mistake already made once.

# No VENDOR dtoverlay= or dtparam= lines. Every overlay in the vendor tree is
# written against Linux driver bindings; NextBSD reads the same device tree
# with its own drivers, and an overlay that renames or reparents a node moves
# it out from under the FreeBSD driver's compatible string. Add them one at a
# time, each with a boot that proves it.
#
# nextbsd-fkms is ours, and meets that bar (nextbsd#429). It does not rename or
# reparent anything -- it flips one node's status from "disabled" to "okay":
#
#   ofwbus0: <firmwarekms> irq 12 disabled compat raspberrypi,rpi-firmware-kms-2712 (no driver attached)
#
# is what a Pi 5 reports without it, with VideoCoreKMS.kext installed and
# loadable but unable to bind. Proven by a boot on a Pi 500+: with the overlay
# staged, the node comes up enabled, the kext attaches, /dev/dri/card0 appears
# and vblank is delivered.
#
# Costs nothing on a board without the node -- an overlay whose target is
# absent is a no-op.
dtoverlay=nextbsd-fkms

# nextbsd-v3d enables v3d@2000000, which also ships disabled. Measured on a Pi
# 500+: the node comes up with the "disabled" gone and IRQs 70/71 resolved, and
# enabling it disturbs neither the firmware framebuffer nor firmware KMS -- fb0
# still comes up and vc4_fkms0 still attaches with /dev/dri/card0.
#
# It does NOT by itself give you 3D. Nothing in tree binds "brcm,2712-v3d", so
# /dev/dri stays card0 with no renderD128. GL additionally needs a v3d DRM
# driver, the drm_gpu_scheduler it depends on, and mesa-libs built with vc4 and
# v3d in GALLIUM_DRIVERS (nextbsd#427). Enabled now because the node has to be
# up before any of that can be developed against it.
dtoverlay=nextbsd-v3d

# nextbsd-vc4-kms enables the seven nodes the full KMS port needs: the vc6
# master, hvs, both pixelvalves, both HDMI controllers and ddc0. Measured on a
# Pi 500+: all seven come up with their IRQs assigned -- HVS with exactly 3
# (ch0/1/2-eof) and each HDMI with exactly 5 -- and firmware KMS still attached
# alongside them.
#
# Enabled, deliberately, even though vc4_kms.ko is not packaged yet: the driver
# is under active development on this hardware
# (nextbsd-kernel-extensions#51) and the nodes have to be enabled for it to
# bind at all. Shipping it disabled would mean hand-editing this file on the
# FAT32 partition after every image rebuild.
#
# It is harmless while nothing binds those nodes. It becomes MUTUALLY EXCLUSIVE
# with nextbsd-fkms the moment vc4_kms.ko is installed, because both drive the
# same display -- at which point exactly one of vc4_fkms and vc4_kms may be
# loaded. That is a driver/packaging decision, not a device-tree one, and this
# comment is where to start when it has to be made.
dtoverlay=nextbsd-vc4-kms
CFG

    # FreeBSD's FDT bootargs parser takes the "FreeBSD:" prefix and reads what
    # follows as boot flags, and -v is the flag worth having on a board whose
    # serial log is the only instrument there is.
    #
    # Measured caveat: on this firmware it does not currently arrive.
    # parse_fdt_bootargs() only parses when fdt_get_chosen_bootargs() succeeds,
    # and a tryboot with exactly this line produced a boot that was not
    # verbose -- so the firmware appears not to be writing /chosen/bootargs at
    # all. Tracked as nextbsd-kernel#93.
    #
    # The file ships anyway. It is correct, it costs nothing, it is the first
    # place a human will look to change a boot flag, and it starts working the
    # day the firmware side is understood. What it is NOT is a channel you can
    # rely on today: anything the kernel must have goes in the compiled-in env
    # instead.
    echo 'FreeBSD: -v' > "$BOOTSTAGE/cmdline.txt"

    # 100 MB FAT32. sectors_per_cluster=1 keeps makefs above the 65525-cluster
    # floor that makes it FAT32 rather than silently falling back to FAT16,
    # which this bootloader will not read.
    echo "==> makefs msdos: rpi500 boot partition"
    makefs -t msdos \
        -o fat_type=32 -o sectors_per_cluster=1 -o volume_label=NEXTBSD \
        -s 102400k \
        "$WORK/rpiboot.img" "$BOOTSTAGE"

    # MBR, not GPT: matching the vendor layout measured on the board's own
    # NVMe (dos label, p1 type 0x0c FAT32-LBA, p2 the OS). fat32lba is that
    # 0x0c; a plain fat32 (0x0b) is CHS-addressed and the wrong type here.
    echo "==> mkimg: MBR disk image (FAT32 boot + freebsd root)"
    # Arch first, board second: NextBSD-arm64-rpi5-<date>.img. Sorts with the
    # other arm64 images, and the globs that consume these are anchored on the
    # datestamp ([0-9]*) so a board token can never be mistaken for one.
    IMG_NAME="NextBSD-${ARCH}-rpi5-${IMG_DATE}.img"
    mkimg -s mbr -f raw \
        -p fat32lba:="$WORK/rpiboot.img" \
        -p freebsd:="$WORK/rootfs.ufs" \
        -o "$WORK/$IMG_NAME"
    ls -lh "$WORK/$IMG_NAME"

    echo "==> zip disk image"
    (cd "$WORK" && zip -9 "$OUT/${IMG_NAME}.zip" "$IMG_NAME")
    ls -lh "$OUT/${IMG_NAME}.zip"
    sha256 "$OUT/${IMG_NAME}.zip" 2>/dev/null || sha256sum "$OUT/${IMG_NAME}.zip"

    rm -f "$WORK/$IMG_NAME" "$WORK/rootfs.ufs" "$WORK/rpiboot.img"
    echo "==> done: $OUT/${IMG_NAME}.zip"
    echo "    No ISO in this lane, by design -- see the comment above."
    exit 0
fi

# 6b. EFI System Partition — FAT, FreeBSD's loader.efi at the UEFI
#     fallback path /EFI/BOOT/BOOTX64.EFI. 33 MB / FAT32, matching
#     FreeBSD's own release images (a smaller ESP drops makefs to
#     FAT12, which some UEFI firmware rejects).
echo "==> building EFI System Partition"
ESPDIR="$WORK/esp-stage"
rm -rf "$ESPDIR"
mkdir -p "$ESPDIR/EFI/BOOT"
# UEFI removable-media fallback name is arch-specific: amd64 -> BOOTX64.EFI,
# arm64 -> BOOTAA64.EFI.
case "$ARCH" in arm64|aarch64) EFIFILE=BOOTAA64.EFI ;; *) EFIFILE=BOOTX64.EFI ;; esac
if [ -f "$WORK/rootfs/boot/loader_lua.efi" ]; then
    cp "$WORK/rootfs/boot/loader_lua.efi" "$ESPDIR/EFI/BOOT/$EFIFILE"
elif [ -f "$WORK/rootfs/boot/loader.efi" ]; then
    cp "$WORK/rootfs/boot/loader.efi" "$ESPDIR/EFI/BOOT/$EFIFILE"
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
echo "==> mkimg: GPT disk image"
# NextBSD-branded, datestamped image name — identical to the published
# continuous asset, so the build output is uploaded + published as-is
# (no rename). boot-test.sh discovers the member by its .img extension.
IMG_NAME="NextBSD-${ARCH}-${IMG_DATE}.img"
# BIOS boot blocks (pmbr/gptboot) only exist on amd64; arm64 is UEFI-only, so
# fall back to an ESP-only GPT there (same arch split the installer uses).
if [ -f "$WORK/rootfs/boot/pmbr" ] && [ -f "$WORK/rootfs/boot/gptboot" ]; then
    echo "    (BIOS + UEFI)"
    mkimg -s gpt -f raw \
        -b "$WORK/rootfs/boot/pmbr" \
        -p freebsd-boot/bootfs:="$WORK/rootfs/boot/gptboot" \
        -p efi/efiboot0:="$WORK/esp.img" \
        -p freebsd-ufs/ROOTFS:="$WORK/rootfs.ufs" \
        -o "$WORK/$IMG_NAME"
else
    echo "    (UEFI-only — no BIOS boot blocks present, e.g. arm64)"
    mkimg -s gpt -f raw \
        -p efi/efiboot0:="$WORK/esp.img" \
        -p freebsd-ufs/ROOTFS:="$WORK/rootfs.ufs" \
        -o "$WORK/$IMG_NAME"
fi
ls -lh "$WORK/$IMG_NAME"

# 6d. compress for publishing — the sparse rw headroom compresses away.
#     Ship the .img.zip: unzip then dd to storage, or boot directly in
#     qemu / VirtualBox / any hypervisor. Zip (vs. gz) opens natively on
#     every platform (Windows Explorer, macOS Finder, Linux file
#     managers) without requiring a separate decompressor; size is
#     within ~0.1% of gz since both use DEFLATE under the hood.
echo "==> zip disk image"
(cd "$WORK" && zip -9 "$OUT/${IMG_NAME}.zip" "$IMG_NAME")
ls -lh "$OUT/${IMG_NAME}.zip"
sha256 "$OUT/${IMG_NAME}.zip" 2>/dev/null || sha256sum "$OUT/${IMG_NAME}.zip"

# trim the GPT image intermediates (the rw rootfs.ufs); $WORK/rootfs itself
# survives — the live ISO below re-uses it for the compressed read-only root.
rm -f "$WORK/$IMG_NAME" "$WORK/rootfs.ufs" "$WORK/esp.img"

#
# 7. live ISO (nextbsd #70 — on-demand compressed root + writable overlay).
#
#    Model (Linux squashfs+overlayfs analog, FreeBSD-native):
#      - rootfs.uzip   : a cd9660 FILE, geom_uzip block-random-access, decompressed
#                        ON READ (NO preload — the 2 GB-into-RAM model is rejected).
#      - a tiny mfsroot (initramfs analog) is the ONLY thing the loader preloads
#        (a few MB of rootfs tools + their lib closure). Its /init assembles:
#            mdconfig -t vnode rootfs.uzip  -> geom_uzip -> /dev/md1.uzip -> /rofs (RO lower)
#            mount -t tmpfs                 -> /cow  (writable upper, swap-backed)
#            mount_unionfs /cow /rofs       -> merged rw view at /rofs
#        then `sysctl vfs.pivot=/rofs` (nextbsd-kernel vfs_pivot.c) adopts the
#        union as the real / — repointing EVERY process's root via the kernel's
#        own mountcheckdirs() — and `exec /sbin/launchd` (PID 1 preserved).
#    The installed rw-UFS disk image (step 6) never unions; only the ISO does.
#
echo "==> live ISO: building on-demand compressed root + mfsroot"

# 7a. Do NOT bake /rofs + /cow mountpoints into the uzip root. The live init
#     mounts the components on the MFSROOT's own /rofs + /cow (created below),
#     and `sysctl vfs.pivot` only repoints rootvnode to the union -- it never
#     relocates those component mounts into the new root (see nextbsd-kernel
#     sys/kern/vfs_pivot.c). Baking them here would just leave empty, orphaned
#     /rofs + /cow dirs in the pivoted / (nextbsd#283); omit them so the live
#     root is clean.

# 7b. Compact UFS image of rootfs (no rw headroom — it is read-only), then
#     mkuzip (zlib; geom_uzip reads it on demand). Separate from the disk
#     image's padded rootfs.ufs.
echo "==> makefs ffs (compact) + mkuzip"
makefs -t ffs -B little -o version=2,label=NBROOT \
    "$WORK/rootfs.iso.ufs" "$WORK/rootfs"
mkuzip -o "$WORK/rootfs.uzip" "$WORK/rootfs.iso.ufs"
ls -lh "$WORK/rootfs.uzip"

# 7c. mfsroot (initramfs): the loader preloads ONLY this. The vmactions FreeBSD
#     VM ships an EMPTY /rescue, so we can't crib the static crunch toolset from
#     it. Instead build a minimal DYNAMIC root from the shipped rootfs's own
#     tools (so they link the rootfs libc we copy alongside): the exact binaries
#     /init needs plus their transitive shared-library closure (flattened into
#     /lib, which is on ld-elf.so.1's default search path). tmpfs/ufs mount via
#     nmount(2) (no helper); cd9660/unionfs use mount_<fs> helpers, which mount(8)
#     execs from _PATH_SYSPATH=/sbin:/bin — both real binaries here.
echo "==> staging mfsroot (rootfs tools + lib closure)"
MFS="$WORK/mfsroot"
RF="$WORK/rootfs"
rm -rf "$MFS"
mkdir -p "$MFS/dev" "$MFS/media" "$MFS/rofs" "$MFS/cow" \
         "$MFS/bin" "$MFS/sbin" "$MFS/lib" "$MFS/libexec"
cp -p "$RF/libexec/ld-elf.so.1" "$MFS/libexec/"
MFS_TOOLS="bin/sh bin/sleep bin/ls sbin/mount sbin/umount sbin/mount_cd9660 sbin/mount_unionfs sbin/mdconfig sbin/sysctl"
for t in $MFS_TOOLS; do
    if [ -f "$RF/$t" ]; then cp -p "$RF/$t" "$MFS/$t"
    else echo "    WARN: mfsroot tool missing in rootfs: $t"; fi
done
# Transitive .so closure: BFS over readelf NEEDED, pulling each soname from the
# rootfs lib dirs into the flat mfsroot /lib.
needed() { readelf -d "$1" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\].*/\1/p'; }
# Fallback search path for a soname the shipped rootfs doesn't provide. The build
# VM's own base is only a legitimate donor on the NATIVE lane: on the cross
# (aarch64-in-an-amd64-VM) lane those are x86-64 objects, and dropping one into
# the aarch64 mfsroot yields a tool that dies in ld-elf.so.1 with "unsupported
# file layout" at live-boot time — a much worse failure to read than the plain
# "shared object not found" the missing lib gives. So the cross lane searches
# only the target rootfs and WARNs if a soname is genuinely absent.
if [ "$ABIARCH" = "$(uname -p)" ]; then
    HOST_LIBDIRS="/lib /usr/lib"
else
    HOST_LIBDIRS=""
    echo "    (cross lane: NOT cribbing libs from the amd64 build VM)"
fi
seen=" "
work=$(for t in $MFS_TOOLS; do [ -f "$MFS/$t" ] && needed "$MFS/$t"; done | sort -u)
while [ -n "$work" ]; do
    nextwork=""
    for so in $work; do
        case "$seen" in *" $so "*) continue ;; esac
        seen="$seen$so "
        # Prefer the shipped rootfs libs; fall back to the build VM's base libs
        # (native lane only — see HOST_LIBDIRS above) for anything the curated
        # base omits (e.g. libkiconv.so.4, which mount_cd9660 hard-NEEDs but the
        # base srclist historically didn't build).
        src=$(find "$RF/lib" "$RF/usr/lib" $HOST_LIBDIRS -name "$so" 2>/dev/null | head -1)
        if [ -n "$src" ]; then
            cp -p "$src" "$MFS/lib/$so"
            nextwork="$nextwork $(needed "$src")"
        else
            echo "    WARN: mfsroot lib STILL not found (rootfs or VM): $so"
        fi
    done
    work=$(printf '%s\n' $nextwork | sort -u)
done
echo "    mfsroot: $(ls "$MFS"/bin "$MFS"/sbin 2>/dev/null | grep -vc :) tools, $(ls "$MFS"/lib 2>/dev/null | wc -l | tr -d ' ') libs"
cat > "$MFS/init" <<'INITEOF'
#!/bin/sh
#
# NextBSD live-ISO init (initramfs analog). Runs as PID 1 from the preloaded
# mfsroot (md0). Assembles a writable overlay over the on-demand compressed
# read-only root, then sysctl vfs.pivot adopts the union as / and execs
# launchd (PID 1 is preserved across exec).
#
# Export the standard default PATH (what the kernel normally hands init), not
# just the mfsroot's /sbin:/bin — launchd inherits this across the exec below and
# propagates it to every login/ssh session, so a truncated PATH here means the
# live shell can't find pkg, /usr tools, etc. The mfsroot's own tools still
# resolve (they're in /sbin:/bin, searched first; the /usr* dirs simply don't
# exist until the pivot, then they're the real union dirs).
# This matches /etc/login.conf's `default` path (minus the per-user ~/bin, which
# the shell expands itself) so the live session's PATH is what the installed
# system would give.
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin:/usr/libexec
LD_LIBRARY_PATH=/lib
export PATH LD_LIBRARY_PATH

mount -t devfs devfs /dev 2>/dev/null
# PID 1 starts with no controlling terminal; wire stdout/stderr to the console
# so these progress lines + any command errors land on the (serial) console.
exec >/dev/console 2>&1
echo "[init] NextBSD live root: assembling overlay"

# Boot media (cd9660). Wait for the CD device / GEOM_LABEL node to settle, then
# try the volume-label node (mkisoimages.sh -b NEXTBSD) first, then raw cd0/cd1.
n=0
while [ ! -e /dev/iso9660/NEXTBSD ] && [ ! -e /dev/cd0 ] && [ "$n" -lt 10 ]; do n=$((n + 1)); sleep 1; done
for dev in /dev/iso9660/NEXTBSD /dev/cd0 /dev/cd1; do
	[ -e "$dev" ] || continue
	if mount -t cd9660 -o ro "$dev" /media 2>&1; then
		echo "[init] media mounted from $dev"; break
	fi
done
echo "[init] cd nodes: $(ls -d /dev/iso9660/* /dev/cd* 2>/dev/null) | /media: $(ls /media 2>/dev/null)"
echo "[init] media: $(ls /media/rootfs.uzip 2>/dev/null || echo rootfs.uzip-MISSING)"

# On-demand compressed root: vnode-back the uzip FILE on the CD (no preload),
# geom_uzip auto-tastes md1 -> /dev/md1.uzip (decompressed on read).
mdconfig -a -t vnode -f /media/rootfs.uzip -u 1
# Wait for geom_uzip to taste md1 and publish the decompressing provider
# /dev/md1.uzip (the GEOM taste runs async on the event queue after mdconfig
# returns). Bounded ~20 s wait so a slow taste doesn't race the mount.
n=0
while [ ! -c /dev/md1.uzip ] && [ "$n" -lt 20 ]; do n=$((n + 1)); sleep 1; done
echo "[init] uzip dev: $(ls -l /dev/md1.uzip 2>/dev/null || echo md1.uzip-ABSENT)"
mount -o ro /dev/md1.uzip /rofs
echo "[init] rofs lower: $(ls -d /rofs/sbin 2>/dev/null || echo /rofs-EMPTY)"

# Single-user (boot -s): the kernel passes "-s" to init (us). Stop HERE, in the
# miniroot, with the read-only compressed base mounted at /rofs so you can
# inspect its contents (the cd9660 is at /media; the uzip provider is
# /dev/md1.uzip). This is BEFORE the tmpfs upper, the unionfs, and the pivot --
# the base is pristine read-only. Exit the shell (^D) to continue the normal
# boot: assemble the writable union, vfs.pivot, and exec launchd.
case " $* " in
*" -s "*)
	echo "[init] ==== single-user (miniroot) ===="
	echo "[init] read-only base: /rofs   boot media: /media   provider: /dev/md1.uzip"
	echo "[init] ^D (exit) continues to multi-user boot."
	/bin/sh </dev/console >/dev/console 2>&1
	echo "[init] resuming boot to multi-user"
	;;
esac

# Writable upper (tmpfs: dynamic, swap-backed) then union lower=/rofs over it.
mount -t tmpfs tmpfs /cow
mount_unionfs /cow /rofs
echo "[init] union assembled; launchd: $(ls /rofs/sbin/launchd 2>/dev/null || echo launchd-MISSING)"

# launchd (the next PID 1) needs a populated /dev on the NEW root. The kernel's
# auto-devfs is on the mfsroot's /dev, which the pivot orphans, so mount a fresh
# devfs on the union's /dev (reachable as /rofs/dev before the pivot) here.
mount -t devfs devfs /rofs/dev

# Adopt the union as the real / (repoints every proc's root) and hand off.
sysctl vfs.pivot=/rofs
echo "[init] pivot complete; exec launchd"
# Drop the mfsroot-only LD_LIBRARY_PATH so launchd + every daemon it spawns use
# the union's normal ld-elf resolution (/lib + /usr/lib via hints), as on a
# regular boot. PATH stays exported (the full default set above).
unset LD_LIBRARY_PATH
exec /sbin/launchd
# If we get here, exec failed — keep PID 1 alive so the panic message is the
# exec error above, not "Going nowhere without my init!".
echo "[init] FATAL: exec /sbin/launchd failed ($?)"
while : ; do sleep 60; done
INITEOF
chmod 0755 "$MFS/init"
# Same root:wheel normalization as the main rootfs: $MFS is assembled by the
# unprivileged build user (mkdir'd dirs + the cat-generated init), so force the
# miniroot tree to uid/gid 0 before makefs bakes it in.
chown -R 0:0 "$MFS"
makefs -t ffs -B little -o version=2,label=MFSROOT -b 3m \
    "$WORK/mfsroot.img" "$MFS"
ls -lh "$WORK/mfsroot.img"

# 7d. ISO bits dir: /boot (loader + kernel, copied from rootfs) + the mfsroot +
#     a live loader config + the uzip. mkisoimages.sh needs boot/cdboot (BIOS El
#     Torito) and boot/loader.efi (UEFI; it builds the El Torito ESP from it).
ISOROOT="$WORK/isoroot"
rm -rf "$ISOROOT"
mkdir -p "$ISOROOT/boot/loader.conf.d" "$ISOROOT/etc"
cp -R "$WORK/rootfs/boot/." "$ISOROOT/boot/"
# cdboot is the BIOS El Torito boot block — amd64 only; arm64 ISOs are UEFI-only
# (booted from loader.efi via the ESP El Torito image). Require cdboot only when
# it exists; loader.efi is required on both.
_isoreq="loader.efi"
[ -f "$ISOROOT/boot/cdboot" ] && _isoreq="cdboot loader.efi"
for f in $_isoreq; do
    [ -f "$ISOROOT/boot/$f" ] || { echo "ERROR: live ISO needs rootfs/boot/$f" >&2; exit 1; }
done
cp "$WORK/mfsroot.img" "$ISOROOT/boot/mfsroot.img"
# mkisoimages.sh runs `makefs -N $ISOROOT/etc` to map owner names -> uid/gid.
for f in passwd group master.passwd; do
    [ -f "$WORK/rootfs/etc/$f" ] && cp "$WORK/rootfs/etc/$f" "$ISOROOT/etc/$f"
done
# Live loader config (zz- => read last, wins). Preload ONLY the mfsroot as md0,
# boot it as the (temporary) root, and run /rescue/init instead of launchd.
# /rescue/init does the on-demand uzip + union + vfs.pivot, then execs launchd.
cat > "$ISOROOT/boot/loader.conf.d/zz-live.conf" <<'LIVEEOF'
# NextBSD live ISO: tiny mfsroot assembles an on-demand compressed root + overlay.
mfsroot_load="YES"
mfsroot_type="md_image"
mfsroot_name="/boot/mfsroot.img"
init_path="/init"
vfs.root.mountfrom="ufs:/dev/md0"
LIVEEOF
cp "$WORK/rootfs.uzip" "$ISOROOT/rootfs.uzip"

# 7e. Build the bootable cd9660 (BIOS cdboot + UEFI ESP) via the release script
#     (extracted from src.txz at step 3a; lands under usr/src/ — locate by glob).
ISO_NAME="NextBSD-${ARCH}-${IMG_DATE}.iso"
# amd64 gets BIOS (El Torito cdboot) + UEFI; arm64 is UEFI-only, same split as
# the GPT disk image above.
echo "==> mkisoimages.sh: bootable cd9660 ($([ -f "$ISOROOT/boot/cdboot" ] && echo 'BIOS + UEFI' || echo 'UEFI-only'))"
MKISO=$(find "$WORK/freebsd-src" -path "*/release/${ARCH}/mkisoimages.sh" 2>/dev/null | head -1)
[ -n "$MKISO" ] || { echo "ERROR: mkisoimages.sh not found under $WORK/freebsd-src" >&2; exit 1; }
# TARGET=$ARCH is REQUIRED for the cross (arm64) lane. release/arm64/mkisoimages.sh
# builds the El Torito ESP with make_esp_file() and lets it derive the UEFI
# fallback loader name from tools/boot/install-boot.sh's get_uefi_bootname(),
# which resolves the target as ${TARGET:-$(uname -m)}. Unset, that is this amd64
# VM -> the aarch64 ISO ships /EFI/BOOT/bootx64.efi, arm64 firmware finds no
# BOOTAA64.EFI, and the ISO does not boot — the exact failure that got the arm64
# lane disabled in #360. (release/amd64/mkisoimages.sh passes `bootx64`
# explicitly, so this is a no-op on the native lane.)
env TARGET="$ARCH" sh "$MKISO" -b NEXTBSD "$WORK/$ISO_NAME" "$ISOROOT"
ls -lh "$WORK/$ISO_NAME"
echo "==> zip live ISO"
(cd "$WORK" && zip -9 "$OUT/${ISO_NAME}.zip" "$ISO_NAME")
ls -lh "$OUT/${ISO_NAME}.zip"
sha256 "$OUT/${ISO_NAME}.zip" 2>/dev/null || sha256sum "$OUT/${ISO_NAME}.zip"

# trim ISO intermediates.
rm -rf "$ISOROOT" "$MFS"
rm -f "$WORK/rootfs.iso.ufs" "$WORK/rootfs.uzip" "$WORK/mfsroot.img" "$WORK/$ISO_NAME"

echo
echo "==> disk image:    $(ls -lh "$OUT/${IMG_NAME}.zip" | awk '{print $5}')  (${IMG_NAME}.zip, DEFLATE-9)"
echo "==> live ISO:      $(ls -lh "$OUT/${ISO_NAME}.zip" | awk '{print $5}')  (${ISO_NAME}.zip, DEFLATE-9)"
echo "==> DONE"
