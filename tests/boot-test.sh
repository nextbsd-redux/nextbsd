#!/bin/sh
# boot-test.sh — boot the disk image in qemu (UEFI), log in as root, and
# run the on-image test suite interactively. CI boots UEFI via OVMF;
# BIOS boot of the same dual-boot image is verified separately.

set -eu

IMG=${1:?usage: boot-test.sh path/to/disk.img[.zip|.gz]}

if [ ! -f "$IMG" ]; then
    echo "ERROR: $IMG not found"
    exit 1
fi

mkdir -p tests
LOG=tests/boot.log
EXP=tests/boot.exp

# Accept the published image in any of: raw .img, .zip (current
# published format — single-entry DEFLATE-9 archive containing
# disk.img), .gz (legacy). Extract/decompress to a raw .img that
# qemu can boot as a disk.
case "$IMG" in
*.zip)
    RAW=tests/disk.img
    echo "==> extracting $IMG -> $RAW"
    # -p: write to stdout; pick the first (and only) entry by name.
    # -o on unzip is overwrite — avoid the interactive prompt.
    unzip -p "$IMG" disk.img > "$RAW"
    IMG=$RAW
    ;;
*.gz)
    RAW=tests/disk.img
    echo "==> decompressing $IMG -> $RAW"
    gunzip -c "$IMG" > "$RAW"
    IMG=$RAW
    ;;
esac

echo "==> boot test: $IMG"
ls -lh "$IMG"

# Pick acceleration. KVM if available; TCG fallback.
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

# Find OVMF firmware
OVMF=""
for f in /usr/share/OVMF/OVMF_CODE.fd \
         /usr/share/ovmf/OVMF.fd \
         /usr/share/qemu/OVMF.fd; do
    if [ -f "$f" ]; then
        OVMF="$f"
        break
    fi
done
if [ -z "$OVMF" ]; then
    echo "ERROR: no OVMF firmware found"
    exit 1
fi
echo "==> using UEFI firmware: $OVMF"

export ACCEL_FLAGS OVMF

cat > "$EXP" <<'EOF'
set timeout 480
log_file -a tests/boot.log
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

# Stage 0: at the loader's autoboot countdown, drop into the OK prompt
# and enable the serial console for the kernel. /boot/loader.conf no
# longer sets console/boot_serial/etc. (clean console on real laptops
# and VBox where the floating comconsole UART line was producing junk
# chars at the loader prompt). Under qemu+OVMF -display none the
# loader's default `efi` console routes to serial so we can interact
# from here.
expect {
    timeout {
        puts "\nFAIL: didn't see loader autoboot prompt within 60s"
        exit 1
    }
    -re "Hit \\\[Enter\\\]" { send " " }
    "Booting"                { send " " }
    "FreeBSD/amd64 EFI"      { send " " }
}

expect {
    timeout {
        puts "\nFAIL: didn't reach loader OK prompt within 30s"
        exit 1
    }
    "OK " { puts "\n==> at loader prompt; setting serial console vars" }
}

# Match the echoed command BEFORE matching "OK " to avoid expect races
# where a stale OK from a prior `set` is matched before the loader has
# consumed the current send. Without this, run 26070245676 ate
# `boot_multicons=YES` and merged the next `set` into the partial line.
#
# Also dropped vidconsole from console var: QEMU CI runs with
# -display none, so vidconsole always fails to bind and the loader
# prints "console vidconsole is unavailable" on every boot. comconsole
# alone is sufficient for serial-only capture.
send "set console=comconsole\r"
expect "set console=comconsole"
expect "OK "
send "set boot_serial=YES\r"
expect "set boot_serial=YES"
expect "OK "
send "set comconsole_speed=115200\r"
expect "set comconsole_speed=115200"
expect "OK "
send "set boot_multicons=YES\r"
expect "set boot_multicons=YES"
expect "OK "
# Verbose diagnostic trace toggles. CI-only — the shipped ISO is
# silent by default. The kernel reads mach.debug_enable as a tunable
# (CTLFLAG_RWTUN) at boot; launchd PID 1 and libxpc both read kenv
# "launchd_trace=1" once at startup. Together these gate the [T41-*]
# / [T39-*] trace points; keeping them on for CI gives a paper trail
# for the next regression.
send "set mach.debug_enable=1\r"
expect "set mach.debug_enable=1"
expect "OK "
send "set launchd_trace=1\r"
expect "set launchd_trace=1"
expect "OK "
send "boot\r"

# Stage 1a: capture getty's boot banner. PAM port iter 4 (issue #99)
# restored RunAtLoad on com.apple.hostnamed.plist so hostnamed runs
# at boot. Banner format: "FreeBSD/amd64 (HOSTNAME) (console)".
#
# This check is INFORMATIONAL, not gating. Both daemons (hostnamed
# + getty) have RunAtLoad=true and launchd dispatches them in
# parallel; getty's fork+exec+banner-print path (~10-30ms) finishes
# before hostnamed's fork+exec+synthesize+sethostname path
# (~30-100ms — kenv + SMBIOS + getifaddrs + crypt). The first-boot
# banner consistently loses the race and shows 'Amnesiac'.
#
# What's actually working: kern.hostname IS set to the synthesized
# value before login runs (verified by HOSTNAMED-OK and PAM-LOGIN-OK
# downstream); second-getty-respawn (after logout) shows the
# synthesized name. The banner-at-first-boot is purely cosmetic.
#
# Apple's macOS doesn't hit this because loginwindow reads the
# hostname dynamically; FreeBSD getty captures it at print time.
# Proper fix needs either launchd job ordering (no native mechanism)
# or a getty patch to defer banner until kern.hostname stabilizes —
# both out of scope for the PAM port.
expect {
    timeout {
        puts "\nFAIL: boot banner not seen within 8 minutes"
        exit 1
    }
    -re "FreeBSD/amd64 \\(Amnesiac\\) \\(console\\)" {
        puts "\nWARN: BOOT-BANNER — first-boot banner shows 'Amnesiac' (getty/hostnamed race; cosmetic only)"
    }
    -re "FreeBSD/amd64 \\(\(\[A-Za-z0-9._-\]+\)\\) \\(console\\)" {
        puts "\nOK: BOOT-BANNER-OK — synthesized hostname '$expect_out(1,string)' visible to getty (hostnamed won the race this run)"
    }
}

# Stage 1b: wait for the getty "login:" prompt. Boot is complete:
# loader preloaded mach.ko -> kernel mounts the freebsd-ufs root rw ->
# /sbin/launchd as PID 1 -> getty plist -> login.
expect {
    timeout {
        puts "\nFAIL: 'login:' prompt not seen within 8 minutes"
        exit 1
    }
    "login:" { puts "\nOK: boot reached the login prompt" }
}

# Stage 2: log in as root. The live ISO has no root password, so login
# either drops straight to the shell or asks "Password:" and accepts an
# empty password. Both paths land at a shell prompt; failure is
# "Login incorrect" or silence.
send "root\r"
expect {
    timeout {
        puts "\nFAIL: no response after sending root"
        exit 1
    }
    "Password:" {
        send "\r"
        exp_continue
    }
    "Login incorrect" {
        puts "\nFAIL: root login rejected"
        exit 1
    }
    -re {[#%$] $} { puts "\nOK: at root shell prompt" }
}

# Stage 3: invoke the on-ISO mach smoke test. Test scripts live under
# /usr/tests/<component>/ following the FreeBSD convention. The script
# emits two markers in sequence:
#   MACH-SMOKE-OK / MACH-SMOKE-FAIL          — kernel-side kldstat -m mach
#   LIBSYSTEM-KERNEL-OK / LIBSYSTEM-KERNEL-FAIL — userland test_libmach
# Both must pass.
send "/usr/tests/freebsd-launchd-mach/run.sh\r"
expect {
    timeout {
        puts "\nFAIL: /usr/tests/freebsd-launchd-mach/run.sh timed out"
        exit 1
    }
    "MACH-SMOKE-FAIL" {
        puts "\nFAIL: mach.ko did NOT load on boot"
        exit 1
    }
    "MACH-SMOKE-OK" { puts "\nOK: mach.ko is loaded" }
}
expect {
    timeout {
        puts "\nFAIL: LIBSYSTEM-KERNEL marker not seen"
        exit 1
    }
    "LIBSYSTEM-KERNEL-FAIL" {
        puts "\nFAIL: libsystem_kernel roundtrip failed"
        exit 1
    }
    "LIBSYSTEM-KERNEL-OK" { puts "\nOK: libsystem_kernel works" }
}
expect {
    timeout {
        puts "\nFAIL: MACH-PORT marker not seen"
        exit 1
    }
    "MACH-PORT-FAIL" {
        puts "\nFAIL: mach_port_* round-trip failed"
        exit 1
    }
    "MACH-PORT-OK" { puts "\nOK: mach_port_* round-trip works" }
}
expect {
    timeout {
        puts "\nFAIL: TASK-SPECIAL-PORT marker not seen"
        exit 1
    }
    "TASK-SPECIAL-PORT-FAIL" {
        puts "\nFAIL: task_*_special_port round-trip failed"
        exit 1
    }
    "TASK-SPECIAL-PORT-OK" { puts "\nOK: task_*_special_port round-trip works" }
}
expect {
    timeout {
        puts "\nFAIL: HOST-BOOTSTRAP marker not seen"
        exit 1
    }
    "HOST-BOOTSTRAP-FAIL" {
        puts "\nFAIL: host_set_special_port fallback failed"
        exit 1
    }
    "HOST-BOOTSTRAP-OK" { puts "\nOK: host_set_special_port fallback works" }
}
expect {
    timeout {
        puts "\nFAIL: BOOTSTRAP marker not seen"
        exit 1
    }
    "BOOTSTRAP-FAIL" {
        puts "\nFAIL: bootstrap protocol round-trip failed"
        exit 1
    }
    "BOOTSTRAP-OK" { puts "\nOK: bootstrap protocol round-trip works" }
}
expect {
    timeout {
        puts "\nFAIL: BOOTSTRAP-REMOTE marker not seen"
        exit 1
    }
    "BOOTSTRAP-REMOTE-FAIL" {
        puts "\nFAIL: cross-process bootstrap round-trip failed"
        exit 1
    }
    "BOOTSTRAP-REMOTE-OK" { puts "\nOK: cross-process bootstrap works" }
}
expect {
    timeout {
        puts "\nFAIL: LIBDISPATCH marker not seen"
        exit 1
    }
    "LIBDISPATCH-FAIL" {
        puts "\nFAIL: libdispatch baseline roundtrip failed"
        exit 1
    }
    "LIBDISPATCH-OK" { puts "\nOK: libdispatch baseline works" }
}
expect {
    timeout {
        puts "\nFAIL: LIBDISPATCH-MACH marker not seen"
        exit 1
    }
    "LIBDISPATCH-MACH-FAIL" {
        puts "\nFAIL: libdispatch Mach RECV round-trip failed"
        exit 1
    }
    "LIBDISPATCH-MACH-OK" { puts "\nOK: libdispatch Mach RECV round-trip works" }
}
expect {
    timeout {
        puts "\nFAIL: LIBXPC marker not seen"
        exit 1
    }
    "LIBXPC-FAIL" {
        puts "\nFAIL: libxpc dictionary round-trip failed"
        exit 1
    }
    "LIBXPC-OK" { puts "\nOK: libxpc dictionary round-trip works" }
}
expect {
    timeout {
        puts "\nFAIL: MIG-BUILD marker not seen"
        exit 1
    }
    "MIG-BUILD-FAIL" {
        puts "\nFAIL: mig / migcom failed to run on the ISO"
        exit 1
    }
    "MIG-BUILD-OK" { puts "\nOK: mig + migcom installed and runnable" }
}
expect {
    timeout {
        puts "\nFAIL: FBSDGLUE marker not seen"
        exit 1
    }
    "FBSDGLUE-FAIL" {
        puts "\nFAIL: fbsdglue full set missing or non-functional"
        exit 1
    }
    "FBSDGLUE-OK" { puts "\nOK: srclist-fbsdglue.txt minimal 25-entry set present + /rescue/ absent (#109)" }
}
expect {
    timeout {
        puts "\nFAIL: FILECMD-LEAF marker not seen"
        exit 1
    }
    "FILECMD-LEAF-FAIL" {
        puts "\nFAIL: file_cmds leaf binaries missing or non-functional"
        exit 1
    }
    "FILECMD-LEAF-OK" { puts "\nOK: file_cmds Apple binaries overlaid pkgbase paths, iter 1+2+3 = 17 tools (#111)" }
}
expect {
    timeout {
        puts "\nFAIL: SHELLCMD-LEAF marker not seen"
        exit 1
    }
    "SHELLCMD-LEAF-FAIL" {
        puts "\nFAIL: shell_cmds leaf binaries missing or non-functional"
        exit 1
    }
    "SHELLCMD-LEAF-OK" { puts "\nOK: shell_cmds Apple binaries overlaid + functional, iter 1+2+3+4 = 38 tools (#112)" }
}
expect {
    timeout {
        puts "\nFAIL: TEXTCMD-LEAF marker not seen"
        exit 1
    }
    "TEXTCMD-LEAF-FAIL" {
        puts "\nFAIL: text_cmds leaf binaries missing or non-functional"
        exit 1
    }
    "TEXTCMD-LEAF-OK" { puts "\nOK: text_cmds Apple stream tools overlaid + functional, iter 1+2+3 = 34 tools (#114)" }
}
expect {
    timeout {
        puts "\nFAIL: ADVCMD-LEAF marker not seen"
        exit 1
    }
    "ADVCMD-LEAF-FAIL" {
        puts "\nFAIL: adv_cmds leaf binaries missing or non-functional"
        exit 1
    }
    "ADVCMD-LEAF-OK" { puts "\nOK: adv_cmds iter 1+2+3 (7 Apple tools overlaid) (#113)" }
}
expect {
    timeout {
        puts "\nFAIL: SYSCMD-LEAF marker not seen"
        exit 1
    }
    "SYSCMD-LEAF-FAIL" {
        puts "\nFAIL: system_cmds leaf binaries missing or non-functional"
        exit 1
    }
    "SYSCMD-LEAF-OK" { puts "\nOK: system_cmds iter 1+2 (8 Apple tools overlaid) (#115)" }
}
expect {
    timeout {
        puts "\nFAIL: LAUNCHD-BUILD marker not seen"
        exit 1
    }
    "LAUNCHD-BUILD-FAIL" {
        puts "\nFAIL: /sbin/launchd failed to exec / reject correctly"
        exit 1
    }
    "LAUNCHD-BUILD-OK" { puts "\nOK: /sbin/launchd execs + rejects non-PID-1" }
}
expect {
    timeout {
        puts "\nFAIL: COREFOUNDATION marker not seen"
        exit 1
    }
    "COREFOUNDATION-FAIL" {
        puts "\nFAIL: libCoreFoundation smoke test failed"
        exit 1
    }
    "COREFOUNDATION-OK" { puts "\nOK: libCoreFoundation CFDictionary + plist round-trip works" }
}
expect {
    timeout {
        puts "\nFAIL: LAUNCHCTL-BUILD marker not seen"
        exit 1
    }
    "LAUNCHCTL-BUILD-FAIL" {
        puts "\nFAIL: /bin/launchctl help failed to exec / link"
        exit 1
    }
    "LAUNCHCTL-BUILD-OK" { puts "\nOK: /bin/launchctl execs + prints help" }
}

# LAUNCHCTL-LIST — runtime smoke. Guards against the print_jobs
# NULL-Label segfault (fixed in support/launchctl.c; without the
# fix, the first response entry missing Label crashed the walk).
# Also guards against the deferred D-state-hang concern — run.sh
# runs launchctl in a 30s budget and emits LIST-FAIL if it doesn't
# exit cleanly.
expect {
    timeout {
        puts "\nFAIL: LAUNCHCTL-LIST marker not seen"
        exit 1
    }
    "LAUNCHCTL-LIST-FAIL" {
        puts "\nFAIL: launchctl list did not complete cleanly"
        exit 1
    }
    "LAUNCHCTL-LIST-OK" { puts "\nOK: launchctl list round-trips with launchd" }
}

# Stage 3+ Phase J runtime: syslogd + notifyd RunAtLoad via plists,
# then syslog(1) post + read-back round-trip via Mach IPC into the
# ASL store. See run.sh tail for the test sequence.
expect {
    timeout {
        puts "\nFAIL: NOTIFYD-PROC marker not seen"
        exit 1
    }
    "NOTIFYD-PROC-FAIL" {
        puts "\nFAIL: notifyd not running at boot"
        exit 1
    }
    "NOTIFYD-PROC-OK" { puts "\nOK: notifyd running" }
}
expect {
    timeout {
        puts "\nFAIL: SYSLOGD-PROC marker not seen"
        exit 1
    }
    "SYSLOGD-PROC-FAIL" {
        puts "\nFAIL: syslogd not running at boot"
        exit 1
    }
    "SYSLOGD-PROC-OK" { puts "\nOK: syslogd running" }
}
expect {
    timeout {
        puts "\nFAIL: SYSLOG-RUN marker not seen"
        exit 1
    }
    "SYSLOG-RUN-FAIL" {
        puts "\nFAIL: syslog(1) post/read round-trip failed"
        exit 1
    }
    "SYSLOG-RUN-SKIP" { puts "\nSKIP: syslog round-trip blocked by launch_msg hang (task #41)" }
    "SYSLOG-RUN-OK" { puts "\nOK: syslog round-trip works" }
}

expect {
    timeout {
        puts "\nFAIL: HWREG-PUBSUB marker not seen"
        exit 1
    }
    "HWREG-PUBSUB-FAIL" {
        puts "\nFAIL: hwregd Mach pub/sub round-trip failed"
        exit 1
    }
    "HWREG-PUBSUB-OK" { puts "\nOK: hwregd Mach pub/sub works" }
}

expect {
    timeout {
        puts "\nFAIL: HWREG-RPC marker not seen"
        exit 1
    }
    "HWREG-RPC-FAIL" {
        puts "\nFAIL: hwregd Mach-RPC registry query failed"
        exit 1
    }
    "HWREG-RPC-OK" { puts "\nOK: hwregd Mach-RPC registry query works" }
}

# HWREG-AUTOLOAD — hwregd boot-backlog autoload: after a 60s settle
# window the daemon drains its deferred-match queue with kldload(2)
# and logs the marker. run.sh polls hwregd.stderr for up to 90s, so
# the global 480s timeout above is plenty.
expect {
    timeout {
        puts "\nFAIL: HWREG-AUTOLOAD marker not seen"
        exit 1
    }
    "HWREG-AUTOLOAD-FAIL" {
        puts "\nFAIL: hwregd boot-backlog autoload drain did not run"
        exit 1
    }
    "HWREG-AUTOLOAD-OK" { puts "\nOK: hwregd boot-backlog autoload drain ran" }
}

expect {
    timeout {
        puts "\nFAIL: CONFIGD-STORE marker not seen"
        exit 1
    }
    "CONFIGD-STORE-FAIL" {
        puts "\nFAIL: configd SCDynamicStore round-trip failed"
        exit 1
    }
    "CONFIGD-STORE-OK" { puts "\nOK: configd SCDynamicStore round-trip works" }
}

expect {
    timeout {
        puts "\nFAIL: CONFIGD-NOTIFY marker not seen"
        exit 1
    }
    "CONFIGD-NOTIFY-FAIL" {
        puts "\nFAIL: configd change-notification round-trip failed"
        exit 1
    }
    "CONFIGD-NOTIFY-OK" { puts "\nOK: configd change notifications work" }
}

expect {
    timeout {
        puts "\nFAIL: CONFIGD-PATTERN marker not seen"
        exit 1
    }
    "CONFIGD-PATTERN-FAIL" {
        puts "\nFAIL: configd regex pattern watch failed"
        exit 1
    }
    "CONFIGD-PATTERN-OK" { puts "\nOK: configd regex pattern watches work" }
}

expect {
    timeout {
        puts "\nFAIL: CONFIGD-LIST marker not seen"
        exit 1
    }
    "CONFIGD-LIST-FAIL" {
        puts "\nFAIL: configd key listing failed"
        exit 1
    }
    "CONFIGD-LIST-OK" { puts "\nOK: configd key listing works" }
}

expect {
    timeout {
        puts "\nFAIL: CONFIGD-MULTI marker not seen"
        exit 1
    }
    "CONFIGD-MULTI-FAIL" {
        puts "\nFAIL: configd batch routines failed"
        exit 1
    }
    "CONFIGD-MULTI-OK" { puts "\nOK: configd batch routines work" }
}

expect {
    timeout {
        puts "\nFAIL: SC-STORE marker not seen"
        exit 1
    }
    "SC-STORE-FAIL" {
        puts "\nFAIL: SCDynamicStore client framework round-trip failed"
        exit 1
    }
    "SC-STORE-OK" { puts "\nOK: SCDynamicStore client framework works" }
}

expect {
    timeout {
        puts "\nFAIL: SC-NOTIFY marker not seen"
        exit 1
    }
    "SC-NOTIFY-FAIL" {
        puts "\nFAIL: SCDynamicStore change-notification delivery failed"
        exit 1
    }
    "SC-NOTIFY-OK" { puts "\nOK: SCDynamicStore change notifications work" }
}

expect {
    timeout {
        puts "\nFAIL: SC-RUNLOOP marker not seen"
        exit 1
    }
    "SC-RUNLOOP-FAIL" {
        puts "\nFAIL: SCDynamicStore run-loop-source notifications failed"
        exit 1
    }
    "SC-RUNLOOP-OK" { puts "\nOK: SCDynamicStore run-loop notifications work" }
}

expect {
    timeout {
        puts "\nFAIL: SC-MULTI marker not seen"
        exit 1
    }
    "SC-MULTI-FAIL" {
        puts "\nFAIL: SCDynamicStore batch get/set failed"
        exit 1
    }
    "SC-MULTI-OK" { puts "\nOK: SCDynamicStore batch get/set works" }
}

expect {
    timeout {
        puts "\nFAIL: SC-PREFS marker not seen"
        exit 1
    }
    "SC-PREFS-FAIL" {
        puts "\nFAIL: SCPreferences read/edit/commit failed"
        exit 1
    }
    "SC-PREFS-OK" { puts "\nOK: SCPreferences read/edit/commit works" }
}

expect {
    timeout {
        puts "\nFAIL: SC-PATH marker not seen"
        exit 1
    }
    "SC-PATH-FAIL" {
        puts "\nFAIL: SCPreferences path accessors failed"
        exit 1
    }
    "SC-PATH-OK" { puts "\nOK: SCPreferences path accessors work" }
}

expect {
    timeout {
        puts "\nFAIL: SC-LOCK marker not seen"
        exit 1
    }
    "SC-LOCK-FAIL" {
        puts "\nFAIL: SCPreferences lock failed"
        exit 1
    }
    "SC-LOCK-OK" { puts "\nOK: SCPreferences lock works" }
}

expect {
    timeout {
        puts "\nFAIL: SC-PNOTIFY marker not seen"
        exit 1
    }
    "SC-PNOTIFY-FAIL" {
        puts "\nFAIL: SCPreferences change-notification delivery failed"
        exit 1
    }
    "SC-PNOTIFY-OK" { puts "\nOK: SCPreferences change notifications work" }
}

expect {
    timeout {
        puts "\nFAIL: SC-PLINK marker not seen"
        exit 1
    }
    "SC-PLINK-FAIL" {
        puts "\nFAIL: SCPreferences path links failed"
        exit 1
    }
    "SC-PLINK-OK" { puts "\nOK: SCPreferences path links work" }
}

expect {
    timeout {
        puts "\nFAIL: SC-NETIF marker not seen"
        exit 1
    }
    "SC-NETIF-FAIL" {
        puts "\nFAIL: SCNetworkInterface enumeration failed"
        exit 1
    }
    "SC-NETIF-OK" { puts "\nOK: SCNetworkInterface enumeration works" }
}

expect {
    timeout {
        puts "\nFAIL: SC-NETSVC marker not seen"
        exit 1
    }
    "SC-NETSVC-FAIL" {
        puts "\nFAIL: SCNetworkService / SCNetworkProtocol failed"
        exit 1
    }
    "SC-NETSVC-OK" { puts "\nOK: SCNetworkService + SCNetworkProtocol work" }
}

expect {
    timeout {
        puts "\nFAIL: SC-NETSET marker not seen"
        exit 1
    }
    "SC-NETSET-FAIL" {
        puts "\nFAIL: SCNetworkSet failed"
        exit 1
    }
    "SC-NETSET-OK" { puts "\nOK: SCNetworkSet works" }
}

expect {
    timeout {
        puts "\nFAIL: SC-VLAN marker not seen"
        exit 1
    }
    "SC-VLAN-FAIL" {
        puts "\nFAIL: SCVLANInterface failed"
        exit 1
    }
    "SC-VLAN-OK" { puts "\nOK: SCVLANInterface works" }
}

expect {
    timeout {
        puts "\nFAIL: SC-BOND marker not seen"
        exit 1
    }
    "SC-BOND-FAIL" {
        puts "\nFAIL: SCBondInterface failed"
        exit 1
    }
    "SC-BOND-OK" { puts "\nOK: SCBondInterface works" }
}

expect {
    timeout {
        puts "\nFAIL: SC-BRIDGE marker not seen"
        exit 1
    }
    "SC-BRIDGE-FAIL" {
        puts "\nFAIL: SCBridgeInterface failed"
        exit 1
    }
    "SC-BRIDGE-OK" { puts "\nOK: SCBridgeInterface works" }
}

expect {
    timeout {
        puts "\nFAIL: IOKIT-WALK marker not seen"
        exit 1
    }
    "IOKIT-WALK-FAIL" {
        puts "\nFAIL: libIOKit registry walk failed"
        exit 1
    }
    "IOKIT-WALK-OK" { puts "\nOK: libIOKit registry walk works" }
}

expect {
    timeout {
        puts "\nFAIL: IOKIT-MATCH marker not seen"
        exit 1
    }
    "IOKIT-MATCH-FAIL" {
        puts "\nFAIL: libIOKit properties + matching failed"
        exit 1
    }
    "IOKIT-MATCH-OK" { puts "\nOK: libIOKit properties + matching work" }
}

expect {
    timeout {
        puts "\nFAIL: IOKIT-IOREG marker not seen"
        exit 1
    }
    "IOKIT-IOREG-FAIL" {
        puts "\nFAIL: ioreg(8) failed"
        exit 1
    }
    "IOKIT-IOREG-OK" { puts "\nOK: ioreg(8) works (K1 success marker)" }
}

expect {
    timeout {
        puts "\nFAIL: IOKIT-NOTIFY marker not seen"
        exit 1
    }
    "IOKIT-NOTIFY-FAIL" {
        puts "\nFAIL: libIOKit IONotificationPort + AddMatchingNotification failed"
        exit 1
    }
    "IOKIT-NOTIFY-OK" {
        puts "\nOK: libIOKit IONotificationPort + AddMatchingNotification work (K2)"
    }
}

expect {
    timeout {
        puts "\nFAIL: IPCFG-BOOT marker not seen"
        exit 1
    }
    "IPCFG-BOOT-FAIL" {
        puts "\nFAIL: ipconfigd skeleton did not claim its Mach service"
        exit 1
    }
    "IPCFG-BOOT-OK" {
        puts "\nOK: ipconfigd skeleton up (com.apple.IPConfiguration registered)"
    }
}

# IPCFG-AUTOLOAD-SUB — iter 9 hwregd attach subscription. ipconfigd
# subscribes to org.freebsd.hwregd's raw pub/sub bus at startup so
# NICs that hwregd autoloads ~60s into boot still get DHCP'd. The
# full attach→DHCP path isn't exercised in CI (em is built into the
# kernel here so it's present at startup, not later autoloaded), but
# this marker proves the subscription wiring is up — when a slimmed
# kernel ships in a later iter, the same wiring carries the chain.
expect {
    timeout {
        puts "\nFAIL: IPCFG-AUTOLOAD-SUB marker not seen"
        exit 1
    }
    "IPCFG-AUTOLOAD-SUB-FAIL" {
        puts "\nFAIL: ipconfigd hwregd subscription did not establish"
        exit 1
    }
    "IPCFG-AUTOLOAD-SUB-OK" {
        puts "\nOK: ipconfigd subscribed to hwregd attach events"
    }
}

# IPCFG-ARP — iter 6 RFC 5227 ARP probe on the DHCPOFFER. Fires
# after 3 successful (= no-reply) probes of the offered address;
# precedes IPCFG-BOUND-OK in the timeline (probe runs between
# OFFER and REQUEST). Conflict path would log IPCFG-BOUND-FAIL.
expect {
    timeout {
        puts "\nFAIL: IPCFG-ARP marker not seen"
        exit 1
    }
    "IPCFG-BOUND-FAIL" {
        puts "\nFAIL: ipconfigd DHCPv4 failed before ARP probe completed"
        exit 1
    }
    "IPCFG-ARP-OK" {
        puts "\nOK: ipconfigd RFC 5227 ARP probe clean (no conflict on offered address)"
    }
}

expect {
    timeout {
        puts "\nFAIL: IPCFG-BOUND marker not seen"
        exit 1
    }
    "IPCFG-BOUND-FAIL" {
        puts "\nFAIL: ipconfigd DHCPv4 INIT → BOUND failed"
        exit 1
    }
    "IPCFG-BOUND-OK" {
        puts "\nOK: ipconfigd DHCPv4 INIT → BOUND works (address + route + DNS installed)"
    }
}

expect {
    timeout {
        puts "\nFAIL: IPCFG-STORE marker not seen"
        exit 1
    }
    "IPCFG-STORE-FAIL" {
        puts "\nFAIL: ipconfigd SCDynamicStore publish failed"
        exit 1
    }
    "IPCFG-STORE-OK" {
        puts "\nOK: ipconfigd published State:/Network/Service/.../IPv4 to configd"
    }
}

# IPCFG-DHCP — issue #88. ipconfigd publishes State:/Network/Service/<UUID>/DHCP
# carrying InterfaceName + LeaseStartTime (always), and Option_12
# (host name, only when the lease supplied it). SLIRP doesn't ship
# Option_12 so the marker just proves the dict shape is correct;
# hostnamed iter 3 is the first consumer that reads the value.
expect {
    timeout {
        puts "\nFAIL: IPCFG-DHCP marker not seen"
        exit 1
    }
    "IPCFG-DHCP-FAIL" {
        puts "\nFAIL: ipconfigd /DHCP publish failed"
        exit 1
    }
    "IPCFG-DHCP-OK" {
        puts "\nOK: ipconfigd published State:/Network/Service/.../DHCP (consumer surface for hostnamed iter 3)"
    }
}

# IPCFG-RA — iter 7a IPv6 Router Advertisement + SLAAC. ipconfigd
# sends an ND_ROUTER_SOLICIT to ff02::2 on em0, waits up to 15s for
# an RA, derives a SLAAC address (EUI-64 over the PIO's prefix),
# installs it + a default ::/0 route via the RA's source LL, and
# publishes State:/Network/Service/.../IPv6.
#
# IPCFG-RA-MISS is the soft path: QEMU SLIRP's IPv6 RA support varies
# by qemu version and CLI args. First-round CI accepts MISS so we can
# see whether SLIRP actually responds; later rounds either keep MISS
# acceptable (if SLIRP is silent) or upgrade to OK-required (if SLIRP
# does answer). Either way, IPv6 not coming up does NOT fail the
# overall boot — IPv4 already passed.
expect {
    timeout {
        puts "\nFAIL: IPCFG-RA marker not seen (neither OK nor MISS)"
        exit 1
    }
    "IPCFG-RA-OK" {
        puts "\nOK: ipconfigd RA/SLAAC works (address + ::/0 route + State:/.../IPv6 published)"
    }
    "IPCFG-RA-MISS" {
        puts "\nWARN: ipconfigd RA-MISS — SLIRP did not respond to RS (IPv4 still OK)"
    }
}

expect {
    timeout {
        puts "\nFAIL: IPCFG-RENEW-OK marker not seen (iter 5b lease renewal CI gate)"
        exit 1
    }
    "IPCFG-RENEW-OK" {
        puts "\nOK: ipconfigd lease renewal works (RENEWING DHCPREQUEST → ACK, lease re-published)"
    }
}

# IPCFG-RPC fires AFTER the daemon log dump (ipconfigrpctest runs at
# the end of run.sh), so expect for it last.
expect {
    timeout {
        puts "\nFAIL: IPCFG-RPC marker not seen"
        exit 1
    }
    "IPCFG-RPC-FAIL" {
        puts "\nFAIL: ipconfigd MIG RPC (ipconfig_if_count / if_addr) failed"
        exit 1
    }
    "IPCFG-RPC-OK" {
        puts "\nOK: ipconfigd MIG RPC works (ipconfig_if_count + ipconfig_if_addr returned the BOUND lease)"
    }
}

# MDNS-BOOT — mDNSResponder Mach service liveness. bootstrap_look_up
# for com.apple.mDNSResponder via mdnstest. Proves the Mach surface
# is up; the engine itself is gated separately by MDNS-ENGINE-OK.
expect {
    timeout {
        puts "\nFAIL: MDNS-BOOT marker not seen"
        exit 1
    }
    "MDNS-BOOT-FAIL" {
        puts "\nFAIL: mDNSResponder did not register its Mach service"
        exit 1
    }
    "MDNS-BOOT-OK" {
        puts "\nOK: mDNSResponder Mach service up (com.apple.mDNSResponder registered)"
    }
}

# MDNS-ENGINE — iter 2 mDNS engine. Logged by the daemon itself
# (PosixDaemon.c, after mDNS_Init returns mStatus_NoError) to
# /var/log/mDNSResponder.stderr, which run.sh cats so the marker
# reaches this console.
expect {
    timeout {
        puts "\nFAIL: MDNS-ENGINE marker not seen"
        exit 1
    }
    "MDNS-ENGINE-FAIL" {
        puts "\nFAIL: mDNS_Init returned an error"
        exit 1
    }
    "MDNS-ENGINE-OK" {
        puts "\nOK: mDNSResponder engine up (mDNS_Init + udsserver ready)"
    }
}

# MDNS-DNSSD — iter 3 end-to-end DNS-SD round-trip via libdns_sd.
# dnssdtest registers "_iter3._tcp" / "freebsd-launchd-mach-iter3"
# through libdns_sd's AF_UNIX channel to the daemon, then browses
# for the same type and waits up to 5s for its own registration to
# show up in the browse callback. Proves the engine + uds_daemon +
# libdns_sd stubs all wire correctly end-to-end — the first iter
# that exercises a real DNS-SD round-trip from a client binary.
expect {
    timeout {
        puts "\nFAIL: MDNS-DNSSD marker not seen"
        exit 1
    }
    "MDNS-DNSSD-FAIL" {
        puts "\nFAIL: dnssdtest could not round-trip Register + Browse via libdns_sd"
        exit 1
    }
    "MDNS-DNSSD-OK" {
        puts "\nOK: libdns_sd end-to-end (Register + Browse round-trip via /var/run/mDNSResponder)"
    }
}

# DA-BOOT — DiskArbitration iter 1 liveness. bootstrap_look_up for
# com.apple.DiskArbitration via datest. Iter 1 ships just the daemon
# skeleton (no hwregd subscription / libgeom / framework yet); iter 2+
# adds storage event subscription via Mach RPC to hwregd.
expect {
    timeout {
        puts "\nFAIL: DA-BOOT marker not seen"
        exit 1
    }
    "DA-BOOT-FAIL" {
        puts "\nFAIL: diskarbitrationd did not register its Mach service"
        exit 1
    }
    "DA-BOOT-OK" {
        puts "\nOK: diskarbitrationd Mach service up (com.apple.DiskArbitration registered)"
    }
}

# DA-WATCH — iter 2 hwregd subscription. The daemon's hwreg_subscribe
# layer sends a HWREG_MSG_SUBSCRIBE to org.freebsd.hwregd, waits for
# the ack EVENT, then spawns a thread to log incoming attach/detach
# events. Storage-device names (ada*/da*/nvd*/cd*/mmcsd*) are tagged
# STORAGE in the log. iter 3+ will move to the MIG-served
# hwreg_watch / hwreg_lookup so we can filter at the hwregd side and
# enumerate the current registry state at startup.
expect {
    timeout {
        puts "\nFAIL: DA-WATCH marker not seen"
        exit 1
    }
    "DA-WATCH-FAIL" {
        puts "\nFAIL: diskarbitrationd could not subscribe to org.freebsd.hwregd"
        exit 1
    }
    "DA-WATCH-OK" {
        puts "\nOK: diskarbitrationd subscribed to hwregd's pub/sub bus"
    }
}

# IPCFG-IPCONFIG — iter 8 Apple-shape CLI. Same MIG round-trip
# ipconfigrpctest exercises, but driven through /usr/sbin/ipconfig.
# Validates that the Apple-canonical CLI parses argv, looks up
# com.apple.IPConfiguration, calls ipconfig_if_count + ipconfig_if_addr,
# and prints the expected results.
expect {
    timeout {
        puts "\nFAIL: IPCFG-IPCONFIG marker not seen"
        exit 1
    }
    "IPCFG-IPCONFIG-FAIL" {
        puts "\nFAIL: /usr/sbin/ipconfig CLI did not return the expected ifcount + getifaddr"
        exit 1
    }
    "IPCFG-IPCONFIG-OK" {
        puts "\nOK: /usr/sbin/ipconfig CLI works (Apple-shape getifaddr + ifcount)"
    }
}

# HOSTNAMED — issue #63 iter 1 gate. ROUND 1 in run.sh: no SCPrefs
# ComputerName set, hostnamed synthesizes "${slug}-${suffix}" from
# SMBIOS + NIC MAC. hostnametest (no arg) reads ComputerName back from
# Setup:/System, HostName + LocalHostName from Setup:/Network/HostNames,
# and gethostname(3) from the kernel; emits HOSTNAMED-OK only when all
# three agree AND the value isn't "Amnesiac". HOSTNAMED-FAIL fires on
# any mismatch / unset / placeholder.
expect {
    timeout {
        puts "\nFAIL: HOSTNAMED marker not seen"
        exit 1
    }
    "HOSTNAMED-FAIL" {
        puts "\nFAIL: hostnamed did not publish a usable hostname"
        exit 1
    }
    "HOSTNAMED-OK" {
        puts "\nOK: hostnamed synthesized + published (SCDynamicStore + kernel agree, value != Amnesiac)"
    }
}

# HOSTNAMED-PREFS — issue #86 iter 2 gate. ROUND 2 in run.sh:
# hostnameprefset writes ComputerName="hostnamed-iter2-fixture" into
# /Library/Preferences/SystemConfiguration/preferences.plist via the
# SCPreferences API; hostnamed re-reads and uses the prefs value
# instead of synthesizing; hostnametest with the fixture arg verifies
# that all three publish surfaces (Setup:/System + Setup:/Network/HostNames
# + kernel) carry exactly that value. HOSTNAMED-PREFS-FAIL fires if
# Tier-2 SCPrefs read didn't fire and synthesis ran anyway.
expect {
    timeout {
        puts "\nFAIL: HOSTNAMED-PREFS marker not seen"
        exit 1
    }
    "HOSTNAMED-PREFS-FAIL" {
        puts "\nFAIL: hostnamed Tier-2 SCPrefs read did not override synthesis"
        exit 1
    }
    "HOSTNAMED-PREFS-OK" {
        puts "\nOK: hostnamed Tier-2 SCPrefs ComputerName beats synthesis (kernel + Setup:/System + Setup:/Network/HostNames all carry the fixture value)"
    }
}

# HOSTNAMED-DHCP — issue #90 iter 3a gate. ROUND 3 in run.sh:
# hostnamedhcpset injects Option_12="hostnamed-iter3a-fixture" into
# the live State:/Network/Service/<UUID>/DHCP dict that ipconfigd
# published (issue #88). With preferences.plist cleared and no kenv
# override, hostnamed's precedence chain falls through to try_dhcp(),
# which finds Option_12 and uses it. hostnametest verifies all three
# publish surfaces + the kernel hostname carry the fixture value
# (proving DHCP beats synthesis but loses to SCPrefs and kenv).
expect {
    timeout {
        puts "\nFAIL: HOSTNAMED-DHCP marker not seen"
        exit 1
    }
    "HOSTNAMED-DHCP-FAIL" {
        puts "\nFAIL: hostnamed Tier-3a DHCP read did not override synthesis"
        exit 1
    }
    "HOSTNAMED-DHCP-OK" {
        puts "\nOK: hostnamed Tier-3a DHCP Option_12 beats synthesis (kernel + Setup:/System + Setup:/Network/HostNames all carry the fixture value)"
    }
}

# PAM-FRAMEWORK — issue #93 iter 1 gate. pamframeworktest exercises
# /usr/lib/libpam.so.6 (our vendored Apple OpenPAM-35) by pam_start
# against /etc/pam.d/test_iter1 (which references pam_deny.so) and
# expects pam_authenticate to return PAM_AUTH_ERR. Proves both the
# framework and the bundled pam_deny.so module load + ABI round-trip.
# Indirectly proves all FreeBSD-runtime PAM consumers (login, su,
# sshd, ...) load our library since soname matches FreeBSD's
# libpam.so.6 exactly — those binaries already authenticated root
# at this point in the boot, so we're in a known-good post-PAM
# state by the time this marker fires.
expect {
    timeout {
        puts "\nFAIL: PAM-FRAMEWORK marker not seen"
        exit 1
    }
    "PAM-FRAMEWORK-FAIL" {
        puts "\nFAIL: Apple OpenPAM framework ABI round-trip broken"
        exit 1
    }
    "PAM-FRAMEWORK-OK" {
        puts "\nOK: Apple OpenPAM libpam.so.6 + pam_deny.so round-trip works (FreeBSD-runtime PAM consumers use our libpam transparently)"
    }
}

# PAM-MODULES — issue #95 iter 2 gate. pammodulestest dlopens each
# of the 5 vendored Apple standalone modules at /usr/lib/pam_NAME.so.6
# and verifies the canonical pam_sm_* entry point is present.
# Indirectly verifies the existing post-login marker chain stays
# green with OUR pam_self.so.6 + pam_uwtmp.so.6 (used by getty/login).
expect {
    timeout {
        puts "\nFAIL: PAM-MODULES marker not seen"
        exit 1
    }
    "PAM-MODULES-FAIL" {
        puts "\nFAIL: one or more Apple PAM standalone modules failed to load or expose required entry points"
        exit 1
    }
    "PAM-MODULES-OK" {
        puts "\nOK: 5 Apple PAM standalone modules (pam_self, pam_rootok, pam_uwtmp, pam_nologin, pam_env) loadable + ABI-valid"
    }
}

# PAM-LOGIN — issue #97 iter 3 gate. `su root -c` round-trip exercises
# our overlay /etc/pam.d/su: pam_rootok (top of auth stack) +
# include system (pam_unix auth/account/password + pam_uwtmp session).
# Existing post-login marker chain (login → MACH-SMOKE-OK → ...
# → HOSTNAMED-DHCP-OK) ALREADY proved login through overlay
# /etc/pam.d/login works at this point, but PAM-LOGIN-OK is the
# explicit named gate that pam_rootok + the system include chain
# round-trip cleanly.
expect {
    timeout {
        puts "\nFAIL: PAM-LOGIN marker not seen"
        exit 1
    }
    "PAM-LOGIN-FAIL" {
        puts "\nFAIL: su round-trip via overlay pam.d/su broke"
        exit 1
    }
    "PAM-LOGIN-OK" {
        puts "\nOK: su round-trip via Apple-source pam.d/su + system stack works (pam_xdg is gone end-to-end)"
    }
}

# Stage 4: clean halt so qemu exits 0 (the -no-reboot flag turns
# halt -p into a clean shutdown rather than a reset loop).
send "halt -p\r"
expect {
    timeout { puts "\nWARN: halt didn't complete within timeout" }
    "Uptime:" { puts "\nOK: clean halt" }
    eof       { puts "\nOK: VM exited" }
}

close
wait
exit 0
EOF

expect "$EXP" "$IMG"
echo "==> boot-test PASSED"
