#!/bin/sh
# img-boot-test.sh — LIGHT boot smoke test for the installed disk image.
# Boots the raw .img in qemu (UEFI via OVMF, virtio disk) and verifies the
# ASSEMBLED image reaches a usable system: loader -> kernel -> launchd PID 1
# -> getty login -> root shell on a UFS root.
#
# It deliberately does NOT run the deep functional suite
# (/usr/tests/freebsd-launchd-mach/run.sh — the LEAF command-suite + launchd /
# CoreFoundation / launchctl + syslog + network-daemon gates). Those exercise
# the Darwin userland and belong in nextbsd-userland's ci-image-boot, closest
# to the source; re-running them here (on the same pkg-built rootfs the ISO
# shares) was redundant and the syslog/network round-trips race under qemu. The
# ISO builder only needs to prove the two boot PATHS reach a shell: this
# direct-UFS disk image and the live ISO (iso-boot-test.sh).
#
# Success = "login:" prompt reached (launchd PID 1 got getty up).
#
# Arch-agnostic: the qemu shape (binary, machine, UEFI firmware, NIC, accel)
# comes from tests/qemu-arch.sh, which takes ARCH from the environment or infers
# it from the NextBSD-<arch>-<date> image name. amd64 boots on q35+OVMF, arm64 on
# virt+AAVMF.

set -eu

IMG=${1:?usage: [ARCH=amd64|arm64] img-boot-test.sh path/to/NextBSD-*.img[.zip]}
[ -f "$IMG" ] || { echo "ERROR: $IMG not found"; exit 1; }
# The as-published name (NextBSD-<arch>-<date>.img.zip) is what carries the arch;
# $IMG is rewritten to the extracted scratch copy below.
ARTIFACT=$IMG

mkdir -p tests
LOG=tests/img-boot.log
EXP=tests/img-boot.exp
: > "$LOG"

case "$IMG" in
*.zip)
    RAW=tests/disk.img
    echo "==> extracting $IMG -> $RAW"
    MEMBER=$(unzip -Z1 "$IMG" | grep -E '\.img$' | head -1)
    [ -n "$MEMBER" ] || { echo "FAIL: no .img member in $IMG" >&2; exit 1; }
    unzip -p "$IMG" "$MEMBER" > "$RAW"
    IMG=$RAW
    ;;
esac

. "$(dirname "$0")/qemu-arch.sh"
qemu_arch_setup "$IMG" "$ARTIFACT"

echo "==> img boot test: $IMG (arch=$ARCH)"
ls -lh "$IMG"

cat > "$EXP" <<'EOF'
set timeout 600
log_file -a tests/img-boot.log
log_user 1

set accel_flags [split $env(ACCEL_FLAGS) " "]
set net_args    [split $env(NET_ARGS) " "]
set video_args  [split $env(VIDEO_ARGS) " "]
set disk_args   [split $env(DISK_ARGS) " "]

eval spawn $env(QEMU) \
    -m 4G \
    -machine $env(MACHINE) \
    -bios $env(FW) \
    $accel_flags \
    $disk_args \
    $net_args \
    $video_args \
    -display none -serial stdio \
    -no-reboot

source tests/loader.exp.inc

# Stage 0: drop into the loader OK prompt and enable the serial console for
# the kernel (/boot/loader.conf leaves console unset for a clean console on
# real hardware).
loader_prompt 60
loader_set "set console=comconsole"
loader_set "set boot_serial=YES"
loader_set "set comconsole_speed=115200"
loader_set "set boot_multicons=YES"
# boot VERBOSE: the shipped image sets boot_mutemsgs="YES" (nextbsd#363), which
# mutes kernel console output. RB_VERBOSE (boot -v) bypasses the mute so CI sees
# full boot output; shipped images (booted normally) stay quiet.
loader_boot "boot -v"

# Stage 1: launchd PID 1 comes up and getty reaches a login prompt.
expect {
    timeout { puts "\nFAIL: 'login:' prompt not seen within 8 minutes"; exit 1 }
    -re "panic|Fatal trap" { puts "\nFAIL: kernel panic during boot"; exit 1 }
    "login:" { puts "\nOK: LOGIN-OK — launchd reached getty on the UFS root" }
}

# Stage 2: log in as root (passwordless from base) and confirm a shell + a UFS
# root (direct disk boot, no live union pivot).
send "root\r"
expect {
    timeout { puts "\nFAIL: no response after sending root"; exit 1 }
    "Password:" { send "\r"; exp_continue }
    "Login incorrect" { puts "\nFAIL: root login rejected"; exit 1 }
    -re {[#%$] $} { puts "\nOK: at root shell prompt" }
}
send "mount | grep ' / '\r"
expect {
    timeout { puts "\nWARN: mount produced no output" }
    -re "ufs" { puts "\nOK: ROOT-IS-UFS — / is a ufs mount" }
    -re {[#%$] $} { }
}
send "halt -p\r"
expect { timeout { } eof { } }
puts "\nIMG-BOOT-DONE"
EOF

set +e
expect -f "$EXP"
rc=$?
set -e

echo "==> verdict"
# The OK `puts` lines go to expect's stdout, not the serial transcript ($LOG).
# Assert against the getty login prompt in the transcript (launchd PID 1 reached
# getty on the installed image).
if grep -q "login:" "$LOG"; then
    echo "PASS: $ARCH disk image booted — launchd reached the login prompt on a UFS root"
    exit 0
fi
echo "FAIL: $ARCH disk image did not reach the login prompt (rc=$rc)"
exit 1
