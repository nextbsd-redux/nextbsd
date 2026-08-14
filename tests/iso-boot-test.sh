#!/bin/sh
# iso-boot-test.sh — boot the LIVE ISO in qemu (UEFI via OVMF, -cdrom) and
# verify the on-demand live-root assembly:
#   loader preloads the mfsroot -> /rescue/init mounts the cd9660, vnode-mds
#   rootfs.uzip (geom_uzip), unions a tmpfs over it, `sysctl vfs.pivot` adopts
#   the union as / -> exec launchd -> getty login prompt.
#
# Success = we see the pivot marker ("vfs.pivot: / is now unionfs") AND the
# login prompt. The full serial log is always dumped for diagnosis — this is
# the feedback loop for iterating the live-root pipeline.
#
# Arch-agnostic: the qemu shape (binary, machine, UEFI firmware, NIC, CD
# attachment, accel) comes from tests/qemu-arch.sh, which takes ARCH from the
# environment or infers it from the NextBSD-<arch>-<date> ISO name.

set -eu

ISO=${1:?usage: [ARCH=amd64|arm64] iso-boot-test.sh path/to/NextBSD-*.iso[.zip]}
[ -f "$ISO" ] || { echo "ERROR: $ISO not found"; exit 1; }
# The as-published name (NextBSD-<arch>-<date>.iso.zip) is what carries the arch;
# $ISO is rewritten to the extracted scratch copy below.
ARTIFACT=$ISO

mkdir -p tests
LOG=tests/iso-boot.log
EXP=tests/iso-boot.exp
: > "$LOG"

case "$ISO" in
*.zip)
    RAW=tests/live.iso
    echo "==> extracting $ISO -> $RAW"
    MEMBER=$(unzip -Z1 "$ISO" | grep -E '\.iso$' | head -1)
    [ -n "$MEMBER" ] || { echo "FAIL: no .iso member in $ISO" >&2; exit 1; }
    unzip -p "$ISO" "$MEMBER" > "$RAW"
    ISO=$RAW
    ;;
esac

. "$(dirname "$0")/qemu-arch.sh"
qemu_arch_setup "$ISO" "$ARTIFACT"

echo "==> iso boot test: $ISO (arch=$ARCH)"
ls -lh "$ISO"

cat > "$EXP" <<'EOF'
set timeout 600
log_file -a tests/iso-boot.log
log_user 1

set accel_flags [split $env(ACCEL_FLAGS) " "]
set net_args    [split $env(NET_ARGS) " "]
set video_args  [split $env(VIDEO_ARGS) " "]
set cd_args     [split $env(CD_ARGS) " "]

eval spawn $env(QEMU) \
    -m 4G \
    -machine $env(MACHINE) \
    -bios $env(FW) \
    $accel_flags \
    $cd_args \
    $net_args \
    $video_args \
    -display none -serial stdio \
    -no-reboot

source tests/loader.exp.inc

# Stage 0: loader autoboot -> OK prompt; enable serial console.
loader_prompt 90
loader_set "set console=comconsole"
loader_set "set boot_serial=YES"
loader_set "set comconsole_speed=115200"
loader_set "set boot_multicons=YES"
# boot VERBOSE: the shipped image sets boot_mutemsgs="YES" (nextbsd#363) which
# mutes kernel console output — including the "vfs.pivot: / is now unionfs"
# marker this harness sequences on. RB_VERBOSE (boot -v) bypasses the mute in
# the kernel, so CI sees all markers while shipped images stay quiet.
loader_boot "boot -v"

# Stage 1: the live-root assembly markers from /rescue/init + vfs.pivot.
set saw_init 0
set saw_pivot 0
expect {
    timeout { puts "\nFAIL: live-root assembly markers not seen within 8 minutes"; exit 1 }
    -re "init\\] NextBSD live root" { set saw_init 1; exp_continue }
    "vfs.pivot: / is now unionfs" {
        set saw_pivot 1
        puts "\nOK: PIVOT-OK — / is now the writable unionfs (on-demand uzip + tmpfs)"
    }
    -re "panic|Fatal trap|vfs.pivot:.*not|mount_unionfs:.*fail|mdconfig:.*" {
        puts "\nWARN: assembly diagnostic: $expect_out(0,string)"
        exp_continue
    }
    "login:" {
        if {$saw_pivot == 0} { puts "\nWARN: reached login WITHOUT a pivot marker (booted mfsroot or fell through?)" }
    }
}

# Stage 2: the login prompt = launchd PID 1 came up on the union.
expect {
    timeout { puts "\nFAIL: 'login:' prompt not seen within 8 minutes"; exit 1 }
    "login:" { puts "\nOK: LOGIN-OK — launchd reached getty on the live union" }
}

# Stage 3: log in, confirm / is a writable union via df.
send "root\r"
expect {
    timeout { puts "\nFAIL: no response after sending root"; exit 1 }
    "Password:" { send "\r"; exp_continue }
    "Login incorrect" { puts "\nFAIL: root login rejected"; exit 1 }
    -re {[#%$] $} { puts "\nOK: at root shell prompt" }
}
send "df / ; mount | grep ' / '\r"
expect {
    timeout { puts "\nWARN: df/mount produced no output" }
    -re "unionfs" { puts "\nOK: ROOT-IS-UNION — / is a unionfs mount" }
    -re {[#%$] $} { }
}
send "halt -p\r"
expect { timeout { } eof { } }
puts "\nISO-BOOT-DONE"
EOF

set +e
expect -f "$EXP"
rc=$?
set -e

echo "==> verdict"
# The PIVOT-OK/LOGIN-OK `puts` lines go to expect's stdout (captured by CI), not
# the spawn transcript ($LOG). Assert against the markers that ARE in the serial
# transcript: the kernel's vfs.pivot adoption + the getty login prompt (launchd
# PID 1 reached getty on the union).
if grep -q "vfs.pivot: / is now unionfs" "$LOG" && grep -q "login:" "$LOG"; then
    echo "PASS: $ARCH live ISO booted — vfs.pivot to writable union + launchd reached the login prompt"
    exit 0
fi
echo "FAIL: $ARCH live ISO did not complete the pivot+login sequence (rc=$rc)"
exit 1
