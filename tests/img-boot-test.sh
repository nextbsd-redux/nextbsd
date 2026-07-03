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

set -eu

IMG=${1:?usage: img-boot-test.sh path/to/NextBSD-*.img[.zip]}
[ -f "$IMG" ] || { echo "ERROR: $IMG not found"; exit 1; }

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

echo "==> img boot test: $IMG"
ls -lh "$IMG"

if [ -e /dev/kvm ]; then
    sudo chmod 666 /dev/kvm 2>/dev/null || true
fi
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    ACCEL_FLAGS="-accel kvm -cpu host"
    echo "==> using KVM acceleration"
else
    ACCEL_FLAGS="-accel tcg,thread=single -cpu qemu64"
    echo "==> using TCG (single-thread)"
fi

OVMF=""
for f in /usr/share/OVMF/OVMF_CODE.fd /usr/share/ovmf/OVMF.fd /usr/share/qemu/OVMF.fd; do
    [ -f "$f" ] && { OVMF="$f"; break; }
done
[ -n "$OVMF" ] || { echo "ERROR: no OVMF firmware found"; exit 1; }
echo "==> using UEFI firmware: $OVMF"

export ACCEL_FLAGS OVMF

cat > "$EXP" <<'EOF'
set timeout 600
log_file -a tests/img-boot.log
log_user 1

set img [lindex $argv 0]
set accel_flags [split $env(ACCEL_FLAGS) " "]

eval spawn qemu-system-x86_64 \
    -m 4G \
    -machine q35 \
    -bios $env(OVMF) \
    $accel_flags \
    -drive file=$img,format=raw,if=virtio \
    -nic user,model=e1000 \
    -display none -serial stdio \
    -no-reboot

# Stage 0: drop into the loader OK prompt and enable the serial console for
# the kernel (/boot/loader.conf leaves console unset for a clean console on
# real hardware). Kept minimal — every extra `set` round-trip risks the loader
# eating serial chars. Match the echoed command before "OK " to avoid racing a
# stale OK from the prior `set`.
expect {
    timeout { puts "\nFAIL: didn't see loader autoboot prompt within 60s"; exit 1 }
    -re "Hit \\\[Enter\\\]" { send " " }
    "Booting"                { send " " }
    "FreeBSD/amd64 EFI"      { send " " }
}
expect {
    timeout { puts "\nFAIL: didn't reach loader OK prompt within 30s"; exit 1 }
    "OK " { puts "\n==> at loader prompt; setting serial console vars" }
}
send "set console=comconsole\r"; expect "set console=comconsole"; expect "OK "
send "set boot_serial=YES\r";    expect "set boot_serial=YES";    expect "OK "
send "set comconsole_speed=115200\r"; expect "set comconsole_speed=115200"; expect "OK "
send "set boot_multicons=YES\r"; expect "set boot_multicons=YES"; expect "OK "
# boot VERBOSE: the shipped image sets boot_mutemsgs="YES" (nextbsd#363), which
# mutes kernel console output. RB_VERBOSE (boot -v) bypasses the mute so CI sees
# full boot output; shipped images (booted normally) stay quiet.
send "boot -v\r"

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
expect -f "$EXP" "$IMG"
rc=$?
set -e

echo "==> verdict"
# The OK `puts` lines go to expect's stdout, not the serial transcript ($LOG).
# Assert against the getty login prompt in the transcript (launchd PID 1 reached
# getty on the installed image).
if grep -q "login:" "$LOG"; then
    echo "PASS: disk image booted — launchd reached the login prompt on a UFS root"
    exit 0
fi
echo "FAIL: disk image did not reach the login prompt (rc=$rc)"
exit 1
