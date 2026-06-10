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

set -eu

ISO=${1:?usage: iso-boot-test.sh path/to/NextBSD-*.iso[.zip]}
[ -f "$ISO" ] || { echo "ERROR: $ISO not found"; exit 1; }

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

echo "==> iso boot test: $ISO"
ls -lh "$ISO"

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
log_file -a tests/iso-boot.log
log_user 1

set iso [lindex $argv 0]
set accel_flags [split $env(ACCEL_FLAGS) " "]

eval spawn qemu-system-x86_64 \
    -m 4G \
    -machine q35 \
    -bios $env(OVMF) \
    $accel_flags \
    -cdrom $iso \
    -boot d \
    -nic user,model=e1000 \
    -display none -serial stdio \
    -no-reboot

# Stage 0: loader autoboot -> OK prompt; enable serial console.
expect {
    timeout { puts "\nFAIL: didn't see loader autoboot prompt within 90s"; exit 1 }
    -re "Hit \\\[Enter\\\]" { send " " }
    "Booting"                { send " " }
    "FreeBSD/amd64 EFI"      { send " " }
    "Loading /boot/loader"   { send " " }
}
expect {
    timeout { puts "\nFAIL: didn't reach loader OK prompt within 30s"; exit 1 }
    "OK " { puts "\n==> at loader prompt; setting serial console vars" }
}
send "set console=comconsole\r"; expect "set console=comconsole"; expect "OK "
send "set boot_serial=YES\r";    expect "set boot_serial=YES";    expect "OK "
send "set comconsole_speed=115200\r"; expect "set comconsole_speed=115200"; expect "OK "
send "set boot_multicons=YES\r"; expect "set boot_multicons=YES"; expect "OK "
send "boot\r"

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
expect -f "$EXP" "$ISO"
rc=$?
set -e

echo "==> verdict"
# The PIVOT-OK/LOGIN-OK `puts` lines go to expect's stdout (captured by CI), not
# the spawn transcript ($LOG). Assert against the markers that ARE in the serial
# transcript: the kernel's vfs.pivot adoption + the getty login prompt (launchd
# PID 1 reached getty on the union).
if grep -q "vfs.pivot: / is now unionfs" "$LOG" && grep -q "login:" "$LOG"; then
    echo "PASS: live ISO booted — vfs.pivot to writable union + launchd reached the login prompt"
    exit 0
fi
echo "FAIL: live ISO did not complete the pivot+login sequence (rc=$rc)"
exit 1
