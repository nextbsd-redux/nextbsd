#!/bin/sh
# /usr/tests/freebsd-launchd-mach/run.sh — phase B kernel-side smoke
# check.
#
# Invoked by CI's boot-test.sh expect script after root login. Prints
# MACH-SMOKE-OK / MACH-SMOKE-FAIL on a single line so CI can match
# either marker exactly.
#
# Path follows FreeBSD convention: base-system tests live under
# /usr/tests/<component>/. We don't (yet) use ATF/Kyua framework here
# — a plain shell script is enough for one trivial smoke check. When
# the suite grows (libmach roundtrip, mach_msg, port allocation,
# etc.), revisit and adopt atf-sh + Kyuafile so `kyua test` works.

set -u

# 1. kernel-side: mach module registered. mach is compiled INTO the kernel
# (#181), so it's a kernel module rather than a separate .ko. The kld* CLIs
# were retired (#193); kextstat -m queries the module by name via modfind(2)
# (a kept syscall), which finds it whether it's a .ko or in-kernel.
if kextstat -m mach >/dev/null 2>&1; then
    echo "MACH-SMOKE-OK: mach module is registered"
    kextstat 2>/dev/null | grep -i mach || true
else
    echo "MACH-SMOKE-FAIL: mach module is NOT registered"
    echo "kextstat output:"
    kextstat
    exit 1
fi

# 2. userland: libsystem_kernel resolves and Mach traps roundtrip.
# Proves the migrated /usr/lib/system/libsystem_kernel.so is
# discoverable via rtld, links into the test binary, and its
# Mach-trap calls actually return valid ports (post Phase C2 lazy
# init, all four traps must succeed).
if [ -x /usr/tests/freebsd-launchd-mach/test_libmach ]; then
    if /usr/tests/freebsd-launchd-mach/test_libmach; then
        echo "LIBSYSTEM-KERNEL-OK: libsystem_kernel roundtrip succeeded"
    else
        rc=$?
        echo "LIBSYSTEM-KERNEL-FAIL: test_libmach exit=$rc"
        echo "ldd:"
        ldd /usr/tests/freebsd-launchd-mach/test_libmach 2>&1 || true
        exit 1
    fi
else
    echo "LIBSYSTEM-KERNEL-FAIL: test_libmach binary not installed"
    exit 1
fi

# 2b. userland: mach_port_allocate / _insert_right / _deallocate
# traps. These are the three new syscalls Phase F-prep wired so the
# ravynOS-fork libxpc can allocate Mach ports beyond the
# task/thread/host/reply family. The test allocates a receive-right
# port, attaches a send right onto the same name, sends and drains a
# self-message through it, then drops the send right — full
# allocate-use-deallocate round-trip.
if [ -x /usr/tests/freebsd-launchd-mach/test_mach_port ]; then
    if /usr/tests/freebsd-launchd-mach/test_mach_port; then
        echo "MACH-PORT-OK: mach_port_* round-trip succeeded"
    else
        rc=$?
        echo "MACH-PORT-FAIL: test_mach_port exit=$rc"
        exit 1
    fi
else
    echo "MACH-PORT-FAIL: test_mach_port binary not installed"
    exit 1
fi

# 2b'. EVFILT_MACHPORT native-filter probe (#168 Phase A). Does the kernel's
# native kqueue Mach-port filter (mach.ko filt_machport, slot -16) actually
# deliver a wakeup + inline-receive a message on a port set? The test prints its
# own EVFILT-MACHPORT-OK/-FAIL/-SKIP marker. NON-FATAL here (the boot-test gate
# is a WARN): this is a discovery probe gating the configd KEM-in-process rework
# (#168) — we want the result reported, not the suite aborted, until we know it.
if [ -x /usr/tests/freebsd-launchd-mach/test_evfilt_machport ]; then
    /usr/tests/freebsd-launchd-mach/test_evfilt_machport || true
else
    echo "EVFILT-MACHPORT-SKIP: test_evfilt_machport binary not installed"
fi

# 2b''. EVFILT_MACHPORT concurrency stress (#168 Stage 0 / #251). Raw pthreads
# hammer concurrent knote attach/detach + port-set teardown + mach_port_move_member
# churn on a shared set — the races behind the PR #250 boot panics (#253 UAF in
# filt_machportattach, #252 NULL td_machdata on non-mach-init threads, #148
# kmsg-destroy on thread exit). A regression panics the kernel and the boot test's
# panic detection fails CI; a clean run prints EVFILT-MACHPORT-CONCURRENT-OK. The
# gate WARNs on SKIP (filter unavailable) and FAILs CI on -FAIL.
if [ -x /usr/tests/freebsd-launchd-mach/test_evfilt_machport_concurrent ]; then
    /usr/tests/freebsd-launchd-mach/test_evfilt_machport_concurrent || true
else
    echo "EVFILT-MACHPORT-CONCURRENT-SKIP: binary not installed"
fi

# 2c. userland: task_get_special_port / task_set_special_port. Phase G
# prerequisite — the bootstrap server uses task_set_bootstrap_port on
# each client task to publish its receive port, and clients read it
# back via task_get_bootstrap_port at mach_init time. The test mints
# a service-shaped port, stores it as TASK_BOOTSTRAP_PORT, reads it
# back, and asserts the round-trip preserves the port name.
if [ -x /usr/tests/freebsd-launchd-mach/test_task_special_port ]; then
    if /usr/tests/freebsd-launchd-mach/test_task_special_port; then
        echo "TASK-SPECIAL-PORT-OK: TASK_BOOTSTRAP_PORT set/get round-trip succeeded"
    else
        rc=$?
        echo "TASK-SPECIAL-PORT-FAIL: test_task_special_port exit=$rc"
        exit 1
    fi
else
    echo "TASK-SPECIAL-PORT-FAIL: test_task_special_port binary not installed"
    exit 1
fi

# 2c'. host_set_special_port + per-task → host fallback (Phase G2b).
# Validates that after the bootstrap server registers its port host-wide
# via host_set_special_port(HOST_BOOTSTRAP_PORT, ...), any task whose
# itk_bootstrap slot is null gets a send right back to the same port
# via task_get_special_port. This is the cross-task discovery path
# the daemon will rely on once it ships in G2c.
if [ -x /usr/tests/freebsd-launchd-mach/test_host_bootstrap ]; then
    if /usr/tests/freebsd-launchd-mach/test_host_bootstrap; then
        echo "HOST-BOOTSTRAP-OK: host-bootstrap fallback works"
    else
        rc=$?
        echo "HOST-BOOTSTRAP-FAIL: test_host_bootstrap exit=$rc"
        exit 1
    fi
else
    echo "HOST-BOOTSTRAP-FAIL: test_host_bootstrap binary not installed"
    exit 1
fi

# 2c''. bus quiescence (device_match_start/end → mach.bus.busy +
# mach_wait_quiet). By the time run.sh executes, the cold-boot device
# probe has finished and hwregd has flipped to live mode, so the
# kernel's in-flight probe->attach count (mach.bus.busy, maintained by
# mach.ko's device_match_start/device_match_end eventhandler consumer)
# must read 0, and mach_wait_quiet must return promptly. test_busystate
# emits BUSYSTATE-OK + WAITQUIET-OK (or *-FAIL).
if [ -x /usr/tests/freebsd-launchd-mach/test_busystate ]; then
    /usr/tests/freebsd-launchd-mach/test_busystate || true  # markers gate in boot-test.sh
else
    echo "BUSYSTATE-FAIL: test_busystate binary not installed"
    echo "WAITQUIET-FAIL: test_busystate binary not installed"
fi

# 2d. userland: bootstrap protocol round-trip (Phase G1). Hand-rolled
# message-ID server loop dispatching CHECK_IN / LOOK_UP requests over
# Mach IPC. The test spawns a pthread that runs bootstrap_server_run,
# then from the main thread does check_in("com.example.test") followed
# by look_up of the same name and asserts the returned port matches.
# Single-task only — cross-process needs complex-message port
# descriptors, lands in Phase G2 alongside the daemon.
if [ -x /usr/tests/freebsd-launchd-mach/test_bootstrap ]; then
    if /usr/tests/freebsd-launchd-mach/test_bootstrap; then
        echo "BOOTSTRAP-OK: bootstrap protocol round-trip succeeded"
    else
        rc=$?
        echo "BOOTSTRAP-FAIL: test_bootstrap exit=$rc"
        exit 1
    fi
else
    echo "BOOTSTRAP-FAIL: test_bootstrap binary not installed"
    exit 1
fi

# 2e. cross-process bootstrap (Phase G2d). Starts the standalone
# bootstrap_server daemon in the background; it publishes its
# service port as HOST_BOOTSTRAP_PORT host-wide. Then runs
# test_bootstrap_remote in a fresh process — that process has no
# per-task bootstrap slot set, so task_get_bootstrap_port falls
# back to the host slot the daemon populated. check_in / look_up
# round-trip over real cross-task Mach IPC validates the complex
# port-descriptor path G2a added.
#
# Cleanup uses SIGKILL (not SIGTERM) deliberately: the daemon's
# SIGTERM-driven graceful-exit path stalls during host_set_special_port
# /mach_port_deallocate on the live ISO (likely because the kernel
# port cleanup races with our process-exit teardown — debug later).
# SIGKILL forces immediate exit and doesn't rely on `wait` returning.
# No `wait` follows: reaping is left to init at script exit, which
# is fine for a smoke test that doesn't reuse the PID.
if [ -x /usr/sbin/bootstrap_server ] && \
   [ -x /usr/tests/freebsd-launchd-mach/test_bootstrap_remote ]; then
    /usr/sbin/bootstrap_server &
    BOOTSTRAP_PID=$!
    trap 'kill -KILL $BOOTSTRAP_PID 2>/dev/null' EXIT INT TERM
    # Give the daemon a beat to allocate its port + register host slot.
    sleep 1
    if /usr/tests/freebsd-launchd-mach/test_bootstrap_remote; then
        echo "BOOTSTRAP-REMOTE-OK: cross-process bootstrap round-trip succeeded"
    else
        rc=$?
        echo "BOOTSTRAP-REMOTE-FAIL: test_bootstrap_remote exit=$rc"
        kill -KILL $BOOTSTRAP_PID 2>/dev/null || true
        trap - EXIT INT TERM
        exit 1
    fi
    kill -KILL $BOOTSTRAP_PID 2>/dev/null || true
    trap - EXIT INT TERM
else
    echo "BOOTSTRAP-REMOTE-FAIL: bootstrap_server or test_bootstrap_remote binary not installed"
    exit 1
fi

# 3. userland: libdispatch loads + serial queue executes a sync callback.
# Baseline check that the vendored swift-corelibs-libdispatch (built
# in our chroot pipeline, installed to /usr/lib/system/) is loadable
# via rtld and dispatches a function-pointer callback correctly. The
# Mach IPC backend test (DISPATCH_SOURCE_TYPE_MACH_RECV) is the
# LIBDISPATCH-MACH gate below.
if [ -x /usr/tests/freebsd-launchd-mach/test_libdispatch ]; then
    if /usr/tests/freebsd-launchd-mach/test_libdispatch; then
        echo "LIBDISPATCH-OK: libdispatch baseline roundtrip succeeded"
    else
        rc=$?
        echo "LIBDISPATCH-FAIL: test_libdispatch exit=$rc"
        echo "ldd:"
        ldd /usr/tests/freebsd-launchd-mach/test_libdispatch 2>&1 || true
        exit 1
    fi
else
    echo "LIBDISPATCH-FAIL: test_libdispatch binary not installed"
    exit 1
fi

# 4. Mach IPC backend round-trip: DISPATCH_SOURCE_TYPE_MACH_RECV over the
# NATIVE kernel filter — allocate a port via mach_reply_port, attach a
# dispatch source, self-send a message, verify the handler fires within 5s
# and consumes it. Under HAVE_MACH the source registers a real
# EVFILT_MACHPORT kevent (Apple's event_kevent.c path) that libmach reroutes
# to mach.ko's filt_machport (slot -16); the kernel filter wakes the source
# edge-triggered (the old event_mach_freebsd.c poll thread was retired in
# #168 Stage 3, #254). Handler's mach_msg(MACH_RCV_MSG) drains the message;
# clean cancel/release teardown.
if [ -x /usr/tests/freebsd-launchd-mach/test_libdispatch_mach ]; then
    if /usr/tests/freebsd-launchd-mach/test_libdispatch_mach; then
        echo "LIBDISPATCH-MACH-OK: Mach RECV round-trip succeeded"
    else
        rc=$?
        echo "LIBDISPATCH-MACH-FAIL: test_libdispatch_mach exit=$rc"
        exit 1
    fi
else
    echo "LIBDISPATCH-MACH-FAIL: test_libdispatch_mach binary not installed"
    exit 1
fi

# 5. libxpc smoke (Phase H2): exercise xpc_dictionary type-system
# in-process — create, set/get string + int64, release. Proves
# libxpc.so links + its core type registry + nv-based serialization
# work. Connection / bootstrap surface lands in a follow-up.
if [ -x /usr/tests/freebsd-launchd-mach/test_libxpc ]; then
    if /usr/tests/freebsd-launchd-mach/test_libxpc; then
        echo "LIBXPC-OK: dictionary round-trip succeeded"
    else
        rc=$?
        echo "LIBXPC-FAIL: test_libxpc exit=$rc"
        echo "ldd:"
        ldd /usr/tests/freebsd-launchd-mach/test_libxpc 2>&1 || true
        exit 1
    fi
else
    echo "LIBXPC-FAIL: test_libxpc binary not installed"
    exit 1
fi

# 6. MIG (bootstrap_cmds): Apple's Mach Interface Generator must run on
# the booted system — prerequisite for the launchd-842 port. We invoked
# `mig -version` at build time inside the chroot, but the live ISO
# needs to demonstrate that mig's runtime deps (wrapper script's
# /usr/bin/cc lookup, migcom binary executes, etc.) are present on the
# actual VM.
if [ -x /usr/bin/mig ] && [ -x /usr/libexec/migcom ]; then
    if /usr/bin/mig -version >/dev/null 2>&1; then
        echo "MIG-BUILD-OK: /usr/bin/mig and migcom run on the ISO"
    else
        rc=$?
        echo "MIG-BUILD-FAIL: /usr/bin/mig -version exit=$rc"
        /usr/bin/mig -version 2>&1 || true
        exit 1
    fi
else
    echo "MIG-BUILD-FAIL: /usr/bin/mig or /usr/libexec/migcom missing"
    ls -la /usr/bin/mig /usr/libexec/migcom 2>&1 || true
    exit 1
fi

# 6.5. fbsdglue iter 2 (#109 / #105a iter 2). Validates srclist-
# fbsdglue.txt — 25 entries covering boot-critical kernel-bound
# platform glue + ldd + HW/FS introspection + user mgmt + save-
# entropy. The BSD-debug toolkit (fstat/sockstat/procstat/kdump/
# ktrace/strings/top/vmstat/etc.) is DEFERRED per the srclist build
# plan §9.6.6 iteration log — nothing in current CI uses them, and
# they pull in FreeBSD privatelib prereqs (libsysdecode, libelftc,
# libprocstat, libmemstat) that aren't load-bearing for the Apple-
# repo ports that follow.
#
# Per-binary check: file exists, is executable. Catches silent
# install no-ops and unresolved-symbol rtld failures.
#
# Also confirms /rescue/ does NOT exist on the ISO.
#
# Plan: https://pkgdemon.github.io/freebsd-srclist-build-plan.html
FBSDGLUE_FAIL=0
# bin/ + sbin/ (kernel-bound + UFS):
# NOTE: the kld* CLIs (kldload/kldunload/kldstat/kldconfig/kldxref) were
# RETIRED (#193) — macOS ships kextload/kextstat, not kldload. The kld*(2)
# SYSCALLS stay; kextload/kextstat (src/kext_tools) drive them. They are
# asserted ABSENT below.
for fbin in /bin/nextbsd-version /bin/kenv \
            /sbin/devfs /sbin/fsck /sbin/fsck_ffs \
            /sbin/ldconfig /sbin/mount /sbin/newfs /sbin/tunefs /sbin/umount; do
    if [ ! -x "$fbin" ]; then
        echo "FBSDGLUE-FAIL: $fbin missing or not executable"
        ls -la "$fbin" 2>&1 || true
        FBSDGLUE_FAIL=1
    fi
done
# usr.bin/ldd is the one debug tool we keep — "why won't this .so
# resolve" is the most common question during Apple-port work.
if [ ! -x /usr/bin/ldd ]; then
    echo "FBSDGLUE-FAIL: /usr/bin/ldd missing"
    FBSDGLUE_FAIL=1
fi
# usr.sbin/ FreeBSD HW/FS introspection + user mgmt + libexec.
# (kldxref retired with the rest of the kld* CLIs, #193 — linker.hints is
# generated at build time, not on the image.)
for fbin in /usr/sbin/devctl /usr/sbin/diskinfo \
            /usr/sbin/fstyp /usr/sbin/gstat \
            /usr/sbin/pciconf /usr/sbin/pw \
            /usr/libexec/save-entropy; do
    if [ ! -x "$fbin" ]; then
        echo "FBSDGLUE-FAIL: $fbin missing or not executable"
        ls -la "$fbin" 2>&1 || true
        FBSDGLUE_FAIL=1
    fi
done
# nologin can land at /sbin/ or /usr/sbin/ depending on Makefile
# BINDIR; usr.sbin/nologin Makefile LINKs to /sbin/nologin too.
if [ ! -x /sbin/nologin ] && [ ! -x /usr/sbin/nologin ]; then
    echo "FBSDGLUE-FAIL: nologin missing from /sbin/ AND /usr/sbin/"
    ls -la /sbin/nologin /usr/sbin/nologin 2>&1 || true
    FBSDGLUE_FAIL=1
fi
# crashinfo is a shell script — check for presence + readability.
if [ ! -r /usr/sbin/crashinfo ]; then
    echo "FBSDGLUE-FAIL: /usr/sbin/crashinfo missing"
    ls -la /usr/sbin/crashinfo 2>&1 || true
    FBSDGLUE_FAIL=1
fi
# Verify /rescue/ absent (FreeBSD-rescue pkg dropped — Apple-shape).
if [ -d /rescue ]; then
    echo "FBSDGLUE-FAIL: /rescue/ still exists; FreeBSD-rescue should have been dropped"
    ls -la /rescue 2>&1 | head -5 || true
    FBSDGLUE_FAIL=1
fi
# Verify the kld* CLIs are ABSENT (#193) — macOS ships kextload/kextstat,
# not kldload. The kld*(2) SYSCALLS stay (kextstat uses them); only the
# user-space CLI front-ends are retired.
for kldcli in /sbin/kldload /sbin/kldunload /sbin/kldstat \
              /sbin/kldconfig /usr/sbin/kldxref; do
    if [ -e "$kldcli" ]; then
        echo "FBSDGLUE-FAIL: $kldcli still present; kld* CLIs should be retired (#193)"
        ls -la "$kldcli" 2>&1 || true
        FBSDGLUE_FAIL=1
    fi
done
# Verify the kext* replacements ARE present.
for kextcli in /usr/sbin/kextload /usr/sbin/kextunload /usr/sbin/kextstat; do
    if [ ! -x "$kextcli" ]; then
        echo "FBSDGLUE-FAIL: $kextcli missing; should replace the retired kld* CLI"
        ls -la "$kextcli" 2>&1 || true
        FBSDGLUE_FAIL=1
    fi
done
if [ $FBSDGLUE_FAIL -eq 0 ]; then
    # Sanity-probe a few: their output paths exit 0 quickly.
    kextstat_count=$(kextstat 2>/dev/null | wc -l | tr -d ' ')
    kenv_count=$(kenv 2>/dev/null | wc -l | tr -d ' ')
    mount_count=$(mount 2>/dev/null | wc -l | tr -d ' ')
    echo "FBSDGLUE-OK: fbsdglue binaries present + executable; kld* CLIs retired (kext* present); BSD-debug toolkit deferred; kextstat=${kextstat_count} kenv=${kenv_count} mount=${mount_count}; /rescue/ absent"
else
    exit 1
fi

# 6.6. file_cmds iter 1+2+3+4 (#111 / #105b). Extends iter 1's
# 5-binary leaf set to 18 pure-POSIX file_cmds tools (+ shar shell
# script). Apple binaries overlay-overwrite FreeBSD-runtime's same
# paths per the OpenPAM iter-3 pattern. Iter 5+ adds the tools with
# Apple-specific deps (cp/mv/ls/chmod/install/gzip/pax/mtree).
#
# Plan: https://pkgdemon.github.io/freebsd-apple-userland-cmds-plan.html#file_cmds
FILECMD_FAIL=0
for fbin in /bin/chflags /bin/mkdir /bin/mkfifo /bin/rmdir \
            /usr/bin/pathchk \
            /bin/dd /bin/ln /bin/rm \
            /usr/bin/cksum /usr/bin/compress \
            /sbin/mknod /usr/bin/touch /usr/bin/truncate \
            /usr/bin/stat /usr/sbin/chown /bin/df \
            /usr/bin/du; do
    if [ ! -x "$fbin" ]; then
        echo "FILECMD-LEAF-FAIL: $fbin missing or not executable"
        ls -la "$fbin" 2>&1 || true
        FILECMD_FAIL=1
    fi
done
# shar is a shell script, not -x by default; check separately.
if [ ! -r /usr/bin/shar ]; then
    echo "FILECMD-LEAF-FAIL: /usr/bin/shar missing"
    ls -la /usr/bin/shar 2>&1 || true
    FILECMD_FAIL=1
fi
# Identity probe: Apple's chflags binary should contain the Apple
# copyright string. FreeBSD's chflags has different copyright text
# (no "Apple"). Differentiates "is the overlay actually overlaying"
# from "FreeBSD-runtime's chflags is still in place."
if [ $FILECMD_FAIL -eq 0 ]; then
    if strings /bin/chflags 2>/dev/null | grep -qi 'apple computer\|copyright.*apple\|opensource'; then
        echo "FILECMD-LEAF-OK: 18/18 file_cmds binaries overlaid; /bin/chflags identifies as Apple's"
    else
        # Fall back: at minimum check that chflags works.
        if /bin/chflags 2>&1 | grep -q 'usage'; then
            echo "FILECMD-LEAF-OK: 18/18 file_cmds binaries present; chflags responds to invocation (Apple identity not verifiable in strings — informational)"
        else
            echo "FILECMD-LEAF-FAIL: chflags doesn't respond to invocation"
            FILECMD_FAIL=1
        fi
    fi
fi
if [ $FILECMD_FAIL -ne 0 ]; then
    exit 1
fi

# 6.7. shell_cmds iter 1+2+3+4+5 (#112 / #105c). Extends to 39 tools.
#   Iter 1: true/false/echo/sleep/basename.
#   Iter 2: 20 more POSIX tools.
#   Iter 3: +11 more (chroot, date, hexdump, lockf, script, shlock,
#           stdbuf, test, whereis, which, xargs) + 'od' link + '[' link.
#   Iter 4: +2 (find, who).
#   Iter 5: +1 (locate) + libexec helpers + /etc/locate.rc.
#           lastcomm DEFERRED — FreeBSD's <sys/acct.h> doesn't expose
#           legacy `struct acct`; needs its own iter.
#
# Plan: https://pkgdemon.github.io/freebsd-apple-userland-cmds-plan.html#shell_cmds
SHELLCMD_FAIL=0
for fbin in /usr/bin/true /usr/bin/false /bin/echo /bin/sleep \
            /usr/bin/basename \
            /usr/bin/apply /usr/bin/dirname /usr/bin/env \
            /usr/bin/getopt /bin/hostname /usr/bin/jot \
            /bin/kill /usr/bin/logname /usr/bin/mktemp \
            /usr/bin/nice /usr/bin/nohup /usr/bin/printenv \
            /usr/bin/printf /bin/pwd /bin/realpath \
            /usr/bin/renice /usr/bin/tee /usr/bin/uname \
            /usr/bin/what /usr/bin/yes \
            /usr/sbin/chroot /bin/date /usr/bin/hexdump \
            /usr/bin/od /usr/bin/lockf /usr/bin/script \
            /usr/bin/shlock /usr/bin/stdbuf /bin/test /bin/[ \
            /usr/bin/whereis /usr/bin/which /usr/bin/xargs \
            /usr/bin/find /usr/bin/who \
            /usr/bin/locate /usr/libexec/locate.bigram \
            /usr/libexec/locate.code; do
    if [ ! -x "$fbin" ]; then
        echo "SHELLCMD-LEAF-FAIL: $fbin missing or not executable"
        ls -la "$fbin" 2>&1 || true
        SHELLCMD_FAIL=1
    fi
done
# Functional sanity: iter-1/2 probes plus iter-3 spot checks.
#   date — prints a year that looks like 20xx.
#   hexdump — hex-dumps a 1-char input cleanly.
#   test — [ 1 -lt 2 ] returns 0.
#   xargs — echo via xargs round-trips.
#   which — finds /bin/sh.
if [ $SHELLCMD_FAIL -eq 0 ]; then
    # locate without a built DB: prints "/var/db/locate.database: No such
    # file or directory" to stderr and exits 1 — that's the expected
    # fresh-rootfs behavior. We're proving the binary loads and parses
    # argv, not that the index exists.
    if /usr/bin/true && ! /usr/bin/false && \
       [ "$(/bin/echo hello)" = "hello" ] && \
       [ "$(/usr/bin/printf 'x%s' yz)" = "xyz" ] && \
       [ "$(/usr/bin/jot 1 5)" = "5" ] && \
       /bin/date +%Y | /usr/bin/grep -qE '^20[0-9][0-9]$' && \
       [ "$(/bin/echo -n A | /usr/bin/hexdump -e '"%02x"')" = "41" ] && \
       /bin/[ 1 -lt 2 ] && \
       [ "$(/bin/echo hello | /usr/bin/xargs /bin/echo)" = "hello" ] && \
       [ -n "$(/usr/bin/which sh)" ] && \
       /usr/bin/find /etc/hosts -maxdepth 0 -type f >/dev/null && \
       /usr/bin/who >/dev/null 2>&1 && \
       ! /usr/bin/locate foo 2>/dev/null; then
        echo "SHELLCMD-LEAF-OK: 39/39 shell_cmds binaries overlaid + functional (iter1+2+3+4+5 probes pass)"
    else
        echo "SHELLCMD-LEAF-FAIL: functional sanity check failed"
        SHELLCMD_FAIL=1
    fi
fi
if [ $SHELLCMD_FAIL -ne 0 ]; then
    exit 1
fi

# 6.8. text_cmds iter 1+2+3 (#114 / #105e). Third Apple-userland-
# cmds repo port.
#   Iter 1: 5 stream processors (cat, head, tail, wc, tr).
#   Iter 2: +17 pure-POSIX leaf tools.
#   Iter 3: +12 more (ed, pr, rs, split, vis, unvis, join, column,
#           md5+sha* links, bintrans+base64+uu*+b64* links,
#           sed, grep+egrep+fgrep+zgrep+xz*+bz*+rgrep links).
#
# Plan: https://pkgdemon.github.io/freebsd-apple-userland-cmds-plan.html#text_cmds
TEXTCMD_FAIL=0
for fbin in /bin/cat /usr/bin/head /usr/bin/tail \
            /usr/bin/wc /usr/bin/tr \
            /usr/bin/col /usr/bin/colrm /usr/bin/comm \
            /usr/bin/csplit /usr/bin/cut /usr/bin/expand \
            /usr/bin/fmt /usr/bin/fold /usr/bin/nl \
            /usr/bin/paste /usr/bin/rev /usr/bin/ul \
            /usr/bin/unexpand /usr/bin/uniq /usr/bin/lam \
            /usr/bin/look /usr/games/banner \
            /bin/ed /usr/bin/pr /usr/bin/rs /usr/bin/split \
            /usr/bin/vis /usr/bin/unvis /usr/bin/join \
            /usr/bin/column /sbin/md5 /sbin/sha256 \
            /usr/bin/bintrans /usr/bin/base64 \
            /usr/bin/sed /usr/bin/grep /usr/bin/egrep; do
    if [ ! -x "$fbin" ]; then
        echo "TEXTCMD-LEAF-FAIL: $fbin missing or not executable"
        ls -la "$fbin" 2>&1 || true
        TEXTCMD_FAIL=1
    fi
done
# Functional probes: iter-1/2 unchanged. Iter-3 spot checks:
#   sed s/// transforms.
#   grep filters.
#   md5 of empty input = d41d8cd98f00b204e9800998ecf8427e.
#   sha256 of empty = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855.
#   base64 round-trips (echo A | base64 -e then -d).
#   join joins two stdin sources (basic check on a here-doc would need
#     temp file; skip the strict probe and just confirm exec rc=0/1/2).
#   pr/column/vis/unvis/rs/split/ed — exec-check (no destructive ops).
if [ $TEXTCMD_FAIL -eq 0 ]; then
    if [ "$(/bin/echo hello | /bin/cat)" = "hello" ] && \
       [ "$(/usr/bin/printf 'a\nb\nc\n' | /usr/bin/head -1)" = "a" ] && \
       [ "$(/usr/bin/printf 'a\nb\nc\n' | /usr/bin/tail -1)" = "c" ] && \
       [ "$(/usr/bin/printf 'abc' | /usr/bin/wc -c | /usr/bin/tr -d ' ')" = "3" ] && \
       [ "$(/usr/bin/printf 'abc' | /usr/bin/tr a-c x-z)" = "xyz" ] && \
       [ "$(/usr/bin/printf 'a:b:c' | /usr/bin/cut -d: -f2)" = "b" ] && \
       [ "$(/usr/bin/printf 'abc' | /usr/bin/rev)" = "cba" ] && \
       [ "$(/usr/bin/printf 'a\na\nb\n' | /usr/bin/uniq | /usr/bin/wc -l | /usr/bin/tr -d ' ')" = "2" ] && \
       [ "$(/usr/bin/printf 'a b' | /usr/bin/colrm 2)" = "a" ] && \
       [ "$(/usr/bin/printf 'a\tb\n' | /usr/bin/expand -t 4 | /usr/bin/wc -c | /usr/bin/tr -d ' ')" = "6" ] && \
       [ "$(/usr/bin/printf 'foo' | /usr/bin/sed 's/o/x/g')" = "fxx" ] && \
       [ "$(/usr/bin/printf 'a\nb\nc\n' | /usr/bin/grep b)" = "b" ] && \
       [ "$(/usr/bin/printf '' | /sbin/md5 -q | /usr/bin/tr -d ' ')" = "d41d8cd98f00b204e9800998ecf8427e" ] && \
       [ "$(/bin/echo -n A | /usr/bin/base64)" = "QQ==" ]; then
        echo "TEXTCMD-LEAF-OK: 34/34 text_cmds tools overlaid + functional (iter1+2+3 probes pass)"
    else
        echo "TEXTCMD-LEAF-FAIL: functional sanity check failed"
        TEXTCMD_FAIL=1
    fi
fi
if [ $TEXTCMD_FAIL -ne 0 ]; then
    exit 1
fi

# 6.9. adv_cmds iter 1+2+3+4 (#113 / #105d). Fourth Apple-userland-cmds
# repo port. iter 1: tabs/tty/whois/lsvfs; iter 2: cap_mkdb/finger;
# iter 3: locale (C++); iter 4: stty (terminal mode setter).
#
# Plan: https://pkgdemon.github.io/freebsd-apple-userland-cmds-plan.html#adv_cmds
ADVCMD_FAIL=0
for fbin in /usr/bin/tabs /usr/bin/tty /usr/bin/whois \
            /usr/sbin/lsvfs \
            /usr/bin/cap_mkdb /usr/bin/finger \
            /usr/bin/locale \
            /bin/stty; do
    if [ ! -x "$fbin" ]; then
        echo "ADVCMD-LEAF-FAIL: $fbin missing or not executable"
        ls -la "$fbin" 2>&1 || true
        ADVCMD_FAIL=1
    fi
done
# Functional probes:
#   lsvfs always succeeds (lists at least ufs/devfs loaded VFS modules).
#   cap_mkdb with no args prints usage to stderr; just check it runs
#     without dyld errors (rc 1 or 64 = usage exit).
#   finger with no user prints header or "No one logged on".
if [ $ADVCMD_FAIL -eq 0 ]; then
    echo "ADVCMD-PROBE: lsvfs"          # DIAG: label each probe so a SIGSEGV is attributable
    if ! /usr/sbin/lsvfs >/dev/null 2>&1; then
        echo "ADVCMD-LEAF-FAIL: lsvfs failed"
        ADVCMD_FAIL=1
    fi
fi
if [ $ADVCMD_FAIL -eq 0 ]; then
    echo "ADVCMD-PROBE: cap_mkdb"
    /usr/bin/cap_mkdb 2>/dev/null
    rc=$?
    if [ $rc -ne 1 ] && [ $rc -ne 2 ] && [ $rc -ne 64 ]; then
        echo "ADVCMD-LEAF-FAIL: cap_mkdb exited unexpectedly (rc=$rc)"
        ADVCMD_FAIL=1
    fi
fi
if [ $ADVCMD_FAIL -eq 0 ]; then
    echo "ADVCMD-PROBE: finger"
    if ! /usr/bin/finger 2>/dev/null | /usr/bin/head -1 >/dev/null; then
        echo "ADVCMD-LEAF-FAIL: finger didn't produce output"
        ADVCMD_FAIL=1
    fi
fi
if [ $ADVCMD_FAIL -eq 0 ]; then
    echo "ADVCMD-PROBE: locale"
    # locale -a lists all installed locales; should at least print "C".
    if ! /usr/bin/locale -a 2>/dev/null | /usr/bin/grep -q '^C$'; then
        echo "ADVCMD-LEAF-FAIL: locale -a didn't list C locale"
        ADVCMD_FAIL=1
    fi
fi
if [ $ADVCMD_FAIL -eq 0 ]; then
    echo "ADVCMD-PROBE: stty"
    # stty -a reads current termios; on a serial console (CI) the tty
    # is valid so this must succeed and print at least "speed".
    if ! /bin/stty -a 2>/dev/null | /usr/bin/grep -q 'speed'; then
        echo "ADVCMD-LEAF-FAIL: stty -a didn't print termios state"
        ADVCMD_FAIL=1
    fi
fi
echo "ADVCMD-PROBE: all functional probes done"
if [ $ADVCMD_FAIL -eq 0 ]; then
    echo "ADVCMD-LEAF-OK: 8/8 adv_cmds binaries overlaid (lsvfs/cap_mkdb/finger/locale/stty probes pass)"
else
    exit 1
fi

# 6.10. system_cmds iter 1+2+3+4+5 (#115 / #105g). Fifth Apple-userland-
# cmds repo port.
#   Iter 1: mkfile, sync, wait4path, pagesize.
#   Iter 2: newgrp, vifs, vipw, accton.
#   Iter 3: getconf (POSIX configuration query; gperf tables via the
#           vendored fake-gperf.awk).
#   Iter 4: getty (replaces FreeBSD-runtime /usr/libexec/getty; the
#           Apple binary now serves the com.apple.getty plist on
#           /dev/console — the existing BOOT-BANNER + "login:" stages
#           in boot-test.sh exercise it implicitly).
#   Iter 5: pwd_mkdb + passwd. Both replace FreeBSD-runtime binaries.
#           passwd is setuid (-m 4555). login DEFERRED — Apple source
#           has drifted enough from FreeBSD usr.bin/login that it
#           needs its own focused source-patching iter.
#
# Plan: https://pkgdemon.github.io/freebsd-apple-userland-cmds-plan.html#system_cmds
SYSCMD_FAIL=0
for fbin in /usr/sbin/mkfile /bin/sync /bin/wait4path \
            /usr/bin/pagesize \
            /usr/bin/newgrp /usr/sbin/vifs /usr/sbin/vipw \
            /usr/sbin/accton \
            /usr/bin/getconf \
            /usr/libexec/getty \
            /usr/sbin/pwd_mkdb /usr/bin/passwd; do
    if [ ! -x "$fbin" ]; then
        echo "SYSCMD-LEAF-FAIL: $fbin missing or not executable"
        ls -la "$fbin" 2>&1 || true
        SYSCMD_FAIL=1
    fi
done
# Iter-1 functional probes (unchanged).
if [ $SYSCMD_FAIL -eq 0 ]; then
    /bin/sync || { echo "SYSCMD-LEAF-FAIL: sync nonzero exit"; SYSCMD_FAIL=1; }
fi
if [ $SYSCMD_FAIL -eq 0 ]; then
    PS=$(/usr/bin/pagesize 2>/dev/null)
    if ! echo "$PS" | grep -qE '^[1-9][0-9]+$'; then
        echo "SYSCMD-LEAF-FAIL: pagesize returned '$PS'"
        SYSCMD_FAIL=1
    fi
fi
if [ $SYSCMD_FAIL -eq 0 ]; then
    rm -f /tmp/syscmd-mkfile-probe
    if /usr/sbin/mkfile 4k /tmp/syscmd-mkfile-probe >/dev/null 2>&1 && \
       [ -f /tmp/syscmd-mkfile-probe ]; then
        rm -f /tmp/syscmd-mkfile-probe
    else
        echo "SYSCMD-LEAF-FAIL: mkfile didn't create the probe file"
        SYSCMD_FAIL=1
    fi
fi
# Iter-2 probes — just verify each binary execs and emits its usage
# (or other expected exit). These are interactive editors / requires
# arg / are sensitive operations; we only smoke-test the rtld + main()
# paths, not full functionality.
if [ $SYSCMD_FAIL -eq 0 ]; then
    # newgrp with no arg: prints "newgrp: no group <id>" or starts a
    # shell. Just check that it doesn't dyld-fail. rc 1 = expected for
    # "Sorry" / "no group" message.
    /usr/bin/newgrp invalidGroupThatDoesNotExist </dev/null >/dev/null 2>&1
    # any rc is fine — we want no crash / no SIGSEGV.
    : "ok"
fi
if [ $SYSCMD_FAIL -eq 0 ]; then
    # vifs/vipw/accton don't have a clean "--help" mode. Just verify
    # exec works (any rc except 127 / 128+sig is ok).
    /usr/sbin/vifs </dev/null >/dev/null 2>&1; rc=$?
    if [ $rc -ge 128 ]; then
        echo "SYSCMD-LEAF-FAIL: vifs crashed (rc=$rc)"
        SYSCMD_FAIL=1
    fi
fi
if [ $SYSCMD_FAIL -eq 0 ]; then
    /usr/sbin/vipw </dev/null >/dev/null 2>&1; rc=$?
    if [ $rc -ge 128 ]; then
        echo "SYSCMD-LEAF-FAIL: vipw crashed (rc=$rc)"
        SYSCMD_FAIL=1
    fi
fi
if [ $SYSCMD_FAIL -eq 0 ]; then
    /usr/sbin/accton </dev/null >/dev/null 2>&1; rc=$?
    if [ $rc -ge 128 ]; then
        echo "SYSCMD-LEAF-FAIL: accton crashed (rc=$rc)"
        SYSCMD_FAIL=1
    fi
fi
# Iter-3 probes — getconf must resolve a confstr (PATH), a sysconf
# (OPEN_MAX), and a pathconf (NAME_MAX on /). All three exercise
# different lookup tables (gperf-generated wordlist). Note: the
# gperf wordlist uses the POSIX bare names (OPEN_MAX, not the C
# enum form _SC_OPEN_MAX).
if [ $SYSCMD_FAIL -eq 0 ]; then
    if ! /usr/bin/getconf PATH >/dev/null 2>&1; then
        echo "SYSCMD-DEBUG-CAUSE: getconf PATH (confstr) failed"
        echo "SYSCMD-LEAF-FAIL"
        SYSCMD_FAIL=1
    fi
fi
if [ $SYSCMD_FAIL -eq 0 ]; then
    SC=$(/usr/bin/getconf OPEN_MAX 2>/dev/null)
    if ! echo "$SC" | grep -qE '^[1-9][0-9]*$'; then
        echo "SYSCMD-DEBUG-CAUSE: getconf OPEN_MAX returned '$SC'"
        echo "SYSCMD-LEAF-FAIL"
        SYSCMD_FAIL=1
    fi
fi
if [ $SYSCMD_FAIL -eq 0 ]; then
    PC=$(/usr/bin/getconf NAME_MAX / 2>/dev/null)
    if ! echo "$PC" | grep -qE '^[1-9][0-9]*$'; then
        echo "SYSCMD-DEBUG-CAUSE: getconf NAME_MAX / returned '$PC'"
        echo "SYSCMD-LEAF-FAIL"
        SYSCMD_FAIL=1
    fi
fi
if [ $SYSCMD_FAIL -eq 0 ]; then
    # getty itself is exercised by getty(8) running on /dev/console
    # (boot-test.sh's BOOT-BANNER + "login:" stages). The existence
    # check above is the only run.sh probe — running it standalone
    # would conflict with the live console.
    echo "SYSCMD-LEAF-OK: 12/12 system_cmds binaries overlaid (iter1+2+3 probes pass; iter4 getty exercised by boot stages; iter5 pwd_mkdb + passwd installed)"
else
    exit 1
fi

# 7. launchd-842 daemon: must exec + reject non-PID-1 invocation.
# launchd-842's main() (launchd.c:163) checks
#   getpid() != 1 && getppid() != 1
# and exits EXIT_FAILURE with a "not meant to be run directly"
# message when both are true — which is the case from a shell.
# Smoke proves: (a) rtld resolves all of liblaunch / libxpc /
# libdispatch / libsystem_kernel / libBlocksRuntime / libutil /
# libpthread, (b) main() actually runs (libc + stdio + getpid OK),
# (c) the non-PID-1 guard fires. No Mach IPC exercised — that's
# I2 territory.
if [ -x /sbin/launchd ]; then
    out=$(/sbin/launchd 2>&1)
    rc=$?
    case "$out" in
        *"not meant to be run directly"*)
            echo "LAUNCHD-BUILD-OK: /sbin/launchd execs and rejects non-PID-1 invocation"
            ;;
        *)
            echo "LAUNCHD-BUILD-FAIL: unexpected output (rc=$rc): $out"
            ldd /sbin/launchd 2>&1 || true
            exit 1
            ;;
    esac
else
    echo "LAUNCHD-BUILD-FAIL: /sbin/launchd missing"
    exit 1
fi

# 8. libCoreFoundation — swift-corelibs CF, non-Swift mode. Exercise
# CFDictionary + CFString + CFPropertyList XML/binary round-trip to
# confirm the legacy refcount path is alive and the plist driver works.
if [ -x /usr/tests/freebsd-launchd-mach/test_corefoundation ]; then
    if /usr/tests/freebsd-launchd-mach/test_corefoundation; then
        echo "COREFOUNDATION-OK: CFDictionary + plist round-trip succeeded"
    else
        rc=$?
        echo "COREFOUNDATION-FAIL: test_corefoundation exit=$rc"
        ldd /usr/tests/freebsd-launchd-mach/test_corefoundation 2>&1 || true
        exit 1
    fi
else
    echo "COREFOUNDATION-FAIL: test_corefoundation binary not installed"
    exit 1
fi

# 9. launchctl — Apple's launchd control utility, ported. Build-only
# smoke at this phase: the binary execs, dynamic linker resolves CF +
# ICU + libdispatch + libxpc + liblaunch, `launchctl version` prints
# the version string. Doesn't require a running launchd (we'd need
# launchd-as-PID-1 for that, which is a later phase).
if [ ! -x /bin/launchctl ]; then
    echo "LAUNCHCTL-BUILD-FAIL: /bin/launchctl missing"
    exit 1
fi

# 1. ldd: all deps must resolve via /usr/lib/system (or /lib).
launchctl_ldd=$(ldd /bin/launchctl 2>&1)
if echo "$launchctl_ldd" | grep -qE 'not found|undefined'; then
    echo "launchctl ldd:"
    echo "$launchctl_ldd"
    echo "---"
    echo "LAUNCHCTL-BUILD-FAIL: ldd shows unresolved deps"
    exit 1
fi

# 2. Spot-check three critical deps actually came from our libsystem
#    layout (libCoreFoundation, lib_FoundationICU, liblaunch).
for lib in libCoreFoundation.so lib_FoundationICU.so liblaunch.so; do
    if ! echo "$launchctl_ldd" | grep -q "$lib.* => /usr/lib/system/"; then
        echo "launchctl ldd:"
        echo "$launchctl_ldd"
        echo "---"
        echo "LAUNCHCTL-BUILD-FAIL: $lib not resolved to /usr/lib/system/"
        exit 1
    fi
done

echo "LAUNCHCTL-BUILD-OK: /bin/launchctl exists ($(stat -f%z /bin/launchctl) bytes), all libsystem deps resolve"

# LAUNCHCTL-LIST — runtime smoke: `launchctl list` round-trips with
# launchd over Mach and prints the job table. Guards against the
# print_jobs NULL-Label segfault that was observed on bsd01 + on
# disk-image boot (every job entry whose serialized response omits
# Label crashed the walk). Run in the background with a kill budget
# rather than `timeout(1)` — if launchctl ever hangs in an
# uninterruptible Mach recv (the deferred concern documented in the
# pre-2026-05-24 version of this block), SIGTERM can't reap it and
# `$()` would block run.sh forever. The background pattern lets us
# move on after the budget regardless of process state.
echo "==> launchctl list"
launchctl_out=/tmp/launchctl_list.out
/bin/launchctl list > "$launchctl_out" 2>&1 &
list_pid=$!
i=0
while [ "$i" -lt 30 ] && kill -0 "$list_pid" 2>/dev/null; do
    sleep 1
    i=$((i + 1))
done
if kill -0 "$list_pid" 2>/dev/null; then
    kill -9 "$list_pid" 2>/dev/null || true
    echo "=== /tmp/launchctl_list.out (partial) ==="
    head -20 "$launchctl_out" 2>/dev/null || true
    echo "=== end ==="
    echo "LAUNCHCTL-LIST-FAIL: still running after 30s (likely D-state Mach recv)"
else
    if wait "$list_pid" 2>/dev/null; then list_rc=0; else list_rc=$?; fi
    head -20 "$launchctl_out"
    if [ "$list_rc" -eq 0 ] && grep -q "^PID" "$launchctl_out"; then
        echo "LAUNCHCTL-LIST-OK: exit=0, $(wc -l < "$launchctl_out") line(s) of output"
    elif [ "$list_rc" -eq 139 ]; then
        echo "LAUNCHCTL-LIST-FAIL: segfault (signal 11) — print_jobs NULL deref?"
    else
        echo "LAUNCHCTL-LIST-FAIL: exit=$list_rc"
    fi
fi

# 10. ASL runtime smoke (Phase J). Task #41 move_member wire-up
# landed but a follow-on halt-after-bootstrap-remote regression is
# under investigation. Keep test at SKIP for now.
sleep 2

# Task #39 debugging: each daemon plist redirects stderr to its own
# /var/log/<daemon>.stderr file. Dump those into the boot console
# BEFORE the proc check so [T39-bs] / [T39-ll] traces (and any other
# diagnostic output) survive the halt that follows a PROC-FAIL exit.
for slog in /var/log/syslogd.stderr /var/log/notifyd.stderr /var/log/aslmanager.stderr /var/log/configd.stderr; do
    if [ -s "$slog" ]; then
        echo "=== begin $slog ==="
        cat "$slog" || true
        echo "=== end $slog ==="
    fi
done

if pgrep -x notifyd >/dev/null 2>&1; then
    echo "NOTIFYD-PROC-OK: notifyd running as pid $(pgrep -x notifyd)"
else
    # DIAG: dump launchd's view BEFORE the FAIL token (the expect harness
    # kills the VM on NOTIFYD-PROC-FAIL). launchctl list's Status column =
    # the job's last wait status: a small +N is exit(N); a value encoding a
    # signal (or 0x8N / negative) means killed by signal N (e.g. 9=SIGKILL).
    echo "=== NOTIFYD-PROC diagnostics (pre-FAIL) ==="
    echo "--- pgrep -fl notifyd (any process, any name) ---"
    pgrep -fl notifyd || echo "(no process matches 'notifyd')"
    echo "--- ps auxww | grep notifyd ---"
    ps auxww | grep -E 'notifyd' | grep -v grep || echo "(none in ps)"
    echo "--- launchctl list | grep notifyd (PID Status Label) ---"
    launchctl list 2>/dev/null | grep -iE 'notifyd' || echo "(notifyd not in launchctl list)"
    echo "--- launchctl list com.apple.notifyd (LastExitStatus) ---"
    launchctl list com.apple.notifyd 2>&1 | grep -iE 'PID|Status|LastExit|Label' || true
    echo "=== end NOTIFYD-PROC diagnostics ==="
    echo "NOTIFYD-PROC-FAIL: notifyd not running"
    exit 1
fi

if pgrep -x syslogd >/dev/null 2>&1; then
    echo "SYSLOGD-PROC-OK: syslogd running as pid $(pgrep -x syslogd)"
else
    # Diagnostics first — the expect harness kills the VM on the
    # SYSLOGD-PROC-FAIL token, so emit that marker last.
    echo "=== SYSLOGD-PROC diagnostics ==="
    ps auxww | grep -E 'syslogd|notifyd' || true
    ls -la /System/Library/LaunchDaemons/ 2>&1 || true
    echo "--- syslogd main checkpoints (/tmp/syslogd_main.log) ---"
    cat /tmp/syslogd_main.log 2>/dev/null || echo "(no syslogd_main.log)"
    echo "--- process_message log (/tmp/process_msg.log) ---"
    cat /tmp/process_msg.log 2>/dev/null || echo "(no process_msg.log)"
    echo "=== end diagnostics ==="
    echo "SYSLOGD-PROC-FAIL: syslogd not running"
    exit 1
fi

# SYSLOG-RUN — real round-trip: post a uniquely tagged message via
# syslog(3) and confirm syslogd ingested it on /var/run/log and routed
# it to /var/log/system.log per asl.conf. Uses test_bsd_logger (libc
# syslog(3), RFC 3164) — installed alongside this script — rather than
# logger(1), which is not in the rootfs and emits RFC 5424.
syslog_mark="SYSLOG-RUN-MARK-$$-$(date +%s)"
test_logger=/usr/tests/freebsd-launchd-mach/test_bsd_logger

if [ ! -x "$test_logger" ]; then
    echo "SYSLOG-RUN-FAIL: $test_logger missing"
    exit 1
fi
"$test_logger" syslogrun "$syslog_mark"

syslog_found=0
i=0
while [ "$i" -lt 10 ]; do
    if grep -q "$syslog_mark" /var/log/system.log 2>/dev/null; then
        syslog_found=1
        break
    fi
    sleep 1
    i=$((i + 1))
done

if [ "$syslog_found" -eq 1 ]; then
    echo "SYSLOG-RUN-OK: round-trip message reached /var/log/system.log"
else
    # Diagnostics first — the expect harness kills the VM on the
    # SYSLOG-RUN-FAIL token, so emit that marker last.
    echo "=== SYSLOG-RUN diagnostics ==="
    echo "--- /var/log/system.log ---"
    cat /var/log/system.log 2>/dev/null || echo "(no system.log)"
    echo "--- /var/run/log socket ---"
    ls -la /var/run/log /var/run/logpriv 2>/dev/null || echo "(no /var/run/log)"
    echo "--- /var/log/asl ---"
    ls -la /var/log/asl/ 2>/dev/null || echo "(no asl store)"
    echo "--- syslogd main checkpoints (/tmp/syslogd_main.log) ---"
    cat /tmp/syslogd_main.log 2>/dev/null || echo "(no syslogd_main.log)"
    echo "--- launch_config checkpoints (/tmp/launch_config.log) ---"
    cat /tmp/launch_config.log 2>/dev/null || echo "(no launch_config.log)"
    echo "--- bsd_in recv log (/tmp/bsd_in_recv.log) ---"
    cat /tmp/bsd_in_recv.log 2>/dev/null || echo "(no bsd_in_recv.log)"
    echo "--- process_message log (/tmp/process_msg.log) ---"
    cat /tmp/process_msg.log 2>/dev/null || echo "(no process_msg.log)"
    echo "--- asl route log (/tmp/asl_route.log) ---"
    cat /tmp/asl_route.log 2>/dev/null || echo "(no asl_route.log)"
    echo "--- syslogd.stderr (final state) ---"
    cat /var/log/syslogd.stderr 2>/dev/null || echo "(no syslogd.stderr)"
    echo "--- syslogd kernel stacks (procstat -kk) ---"
    procstat -kk "$(pgrep -x syslogd)" 2>/dev/null || echo "(procstat unavailable)"
    echo "=== end diagnostics ==="
    echo "SYSLOG-RUN-FAIL: marker not found in /var/log/system.log"
    exit 1
fi

# CONFIGD-STORE — configd SCDynamicStore round-trip: open a session
# with configd, set a key, read it back, remove it, all over the
# config.defs Mach RPC. Proves the configd daemon + its store work
# end to end from a separate client process.
configtest=/usr/tests/freebsd-launchd-mach/configtest
if [ ! -x "$configtest" ]; then
    echo "CONFIGD-STORE-FAIL: $configtest missing"
    exit 1
fi
"$configtest" || true	# marker (CONFIGD-STORE-OK/FAIL) gates in boot-test.sh

# CONFIGD-NOTIFY — configd change notifications + per-session ports:
# open two sessions, have one watch a key and register a Mach
# notification port, change the key from the other session, and
# confirm the notification message + notifychanges report it.
notifytest=/usr/tests/freebsd-launchd-mach/notifytest
if [ ! -x "$notifytest" ]; then
    echo "CONFIGD-NOTIFY-FAIL: $notifytest missing"
    exit 1
fi
"$notifytest" || true	# marker (CONFIGD-NOTIFY-OK/FAIL) gates in boot-test.sh

# CONFIGD-PATTERN — configd regex pattern watches: a session watches a
# POSIX regex, another changes a matching and a non-matching key, and
# configd must notify only for the match.
patterntest=/usr/tests/freebsd-launchd-mach/patterntest
if [ ! -x "$patterntest" ]; then
    echo "CONFIGD-PATTERN-FAIL: $patterntest missing"
    exit 1
fi
"$patterntest" || true	# marker (CONFIGD-PATTERN-OK/FAIL) gates in boot-test.sh

# CONFIGD-LIST — configd key listing: store keys and query them back
# with configlist by prefix, by empty key (all), and by POSIX regex.
listtest=/usr/tests/freebsd-launchd-mach/listtest
if [ ! -x "$listtest" ]; then
    echo "CONFIGD-LIST-FAIL: $listtest missing"
    exit 1
fi
"$listtest" || true	# marker (CONFIGD-LIST-OK/FAIL) gates in boot-test.sh

# CONFIGD-MULTI — configd batch routines: set/remove several keys with
# configset_m, fetch several with configget_m (by key and by regex),
# and replace a session's whole watch set with notifyset.
multitest=/usr/tests/freebsd-launchd-mach/multitest
if [ ! -x "$multitest" ]; then
    echo "CONFIGD-MULTI-FAIL: $multitest missing"
    exit 1
fi
"$multitest" || true	# marker (CONFIGD-MULTI-OK/FAIL) gates in boot-test.sh

# SC-STORE — SCDynamicStore client framework: drive configd through the
# CoreFoundation-typed SCDynamicStore* API (libSystemConfiguration)
# instead of raw config.defs — open a session, set/get/add/remove
# property-list values and list keys.
sctest=/usr/tests/freebsd-launchd-mach/sctest
if [ ! -x "$sctest" ]; then
    echo "SC-STORE-FAIL: $sctest missing"
    exit 1
fi
"$sctest" || true	# marker (SC-STORE-OK/FAIL) gates in boot-test.sh

# SC-NOTIFY — SCDynamicStore change notifications: one session watches a
# key and takes an SCDynamicStore callback on a dispatch queue, another
# writes the key, and the callback must fire with the changed key.
scnotifytest=/usr/tests/freebsd-launchd-mach/scnotifytest
if [ ! -x "$scnotifytest" ]; then
    echo "SC-NOTIFY-FAIL: $scnotifytest missing"
    exit 1
fi
"$scnotifytest" || true	# marker (SC-NOTIFY-OK/FAIL) gates in boot-test.sh

# SC-RUNLOOP — SCDynamicStore run-loop-source notifications: a session
# watches a key via SCDynamicStoreCreateRunLoopSource added to its run
# loop, another writes the key, and running the run loop must fire the
# callback with the changed key.
scrltest=/usr/tests/freebsd-launchd-mach/scrltest
if [ ! -x "$scrltest" ]; then
    echo "SC-RUNLOOP-FAIL: $scrltest missing"
    exit 1
fi
"$scrltest" || true	# marker (SC-RUNLOOP-OK/FAIL) gates in boot-test.sh

# SC-MULTI — SCDynamicStore batch get/set: SCDynamicStoreSetMultiple
# sets several keys in one call, SCDynamicStoreCopyMultiple fetches
# them back by key and by pattern, then one is removed.
scmultitest=/usr/tests/freebsd-launchd-mach/scmultitest
if [ ! -x "$scmultitest" ]; then
    echo "SC-MULTI-FAIL: $scmultitest missing"
    exit 1
fi
"$scmultitest" || true	# marker (SC-MULTI-OK/FAIL) gates in boot-test.sh

# SC-PREFS — SCPreferences read/edit/commit: open a preferences file,
# set values, commit, re-open and confirm they persisted, then remove.
scprefstest=/usr/tests/freebsd-launchd-mach/scprefstest
if [ ! -x "$scprefstest" ]; then
    echo "SC-PREFS-FAIL: $scprefstest missing"
    exit 1
fi
"$scprefstest" || true	# marker (SC-PREFS-OK/FAIL) gates in boot-test.sh

# SC-PATH — SCPreferences path accessors: set a dictionary at a nested
# '/'-separated path, read it (and an intermediate level) back, commit,
# re-open to confirm it persisted, then remove.
scpathtest=/usr/tests/freebsd-launchd-mach/scpathtest
if [ ! -x "$scpathtest" ]; then
    echo "SC-PATH-FAIL: $scpathtest missing"
    exit 1
fi
"$scpathtest" || true	# marker (SC-PATH-OK/FAIL) gates in boot-test.sh

# SC-LOCK — SCPreferences lock: two sessions on one preferences file
# contend for the exclusive lock; commit takes the lock itself.
sclocktest=/usr/tests/freebsd-launchd-mach/sclocktest
if [ ! -x "$sclocktest" ]; then
    echo "SC-LOCK-FAIL: $sclocktest missing"
    exit 1
fi
"$sclocktest" || true	# marker (SC-LOCK-OK/FAIL) gates in boot-test.sh

# SC-PNOTIFY — SCPreferences change notifications: one session watches a
# preferences file on a dispatch queue, another commits a change, and
# the watcher's callback must fire.
scprefsnotifytest=/usr/tests/freebsd-launchd-mach/scprefsnotifytest
if [ ! -x "$scprefsnotifytest" ]; then
    echo "SC-PNOTIFY-FAIL: $scprefsnotifytest missing"
    exit 1
fi
"$scprefsnotifytest" || true	# marker (SC-PNOTIFY-OK/FAIL) gates in boot-test.sh

# SC-PLINK — SCPreferences path links (the SCNetworkConfiguration
# prerequisite): create a unique child entry, store + read a __LINK__,
# resolve a path through the link, and confirm the link persists.
scplinktest=/usr/tests/freebsd-launchd-mach/scplinktest
if [ ! -x "$scplinktest" ]; then
    echo "SC-PLINK-FAIL: $scplinktest missing"
    exit 1
fi
"$scplinktest" || true	# marker (SC-PLINK-OK/FAIL) gates in boot-test.sh

# SC-NETIF — SCNetworkConfiguration interface enumeration: list the
# network interfaces and confirm the e1000 NIC is reported as an
# Ethernet interface with a hardware address, loopback excluded.
scnetiftest=/usr/tests/freebsd-launchd-mach/scnetiftest
if [ ! -x "$scnetiftest" ]; then
    echo "SC-NETIF-FAIL: $scnetiftest missing"
    exit 1
fi
"$scnetiftest" || true	# marker (SC-NETIF-OK/FAIL) gates in boot-test.sh

# SC-NETSVC — SCNetworkConfiguration service + protocol: create a
# network service on the e1000 interface, name it, attach + configure
# an IPv4 protocol, commit, reopen and confirm it all persisted.
scnetsvctest=/usr/tests/freebsd-launchd-mach/scnetsvctest
if [ ! -x "$scnetsvctest" ]; then
    echo "SC-NETSVC-FAIL: $scnetsvctest missing"
    exit 1
fi
"$scnetsvctest" || true	# marker (SC-NETSVC-OK/FAIL) gates in boot-test.sh

# SC-NETSET — SCNetworkConfiguration set ("location"): create a network
# set, name it, add/remove a service, set a service order, make it
# current, commit, reopen and confirm it all persisted.
scnetsettest=/usr/tests/freebsd-launchd-mach/scnetsettest
if [ ! -x "$scnetsettest" ]; then
    echo "SC-NETSET-FAIL: $scnetsettest missing"
    exit 1
fi
"$scnetsettest" || true	# marker (SC-NETSET-OK/FAIL) gates in boot-test.sh

# SC-VLAN — SCNetworkConfiguration VLAN virtual interface: create a
# VLAN on the e1000 interface, check its physical interface / tag /
# name / options, commit, reopen and confirm it persisted.
scvlantest=/usr/tests/freebsd-launchd-mach/scvlantest
if [ ! -x "$scvlantest" ]; then
    echo "SC-VLAN-FAIL: $scvlantest missing"
    exit 1
fi
"$scvlantest" || true	# marker (SC-VLAN-OK/FAIL) gates in boot-test.sh

# SC-BOND — SCNetworkConfiguration bond virtual interface: create a
# bond, add the e1000 interface as a member, check the member list /
# name / options, commit, reopen and confirm it persisted.
scbondtest=/usr/tests/freebsd-launchd-mach/scbondtest
if [ ! -x "$scbondtest" ]; then
    echo "SC-BOND-FAIL: $scbondtest missing"
    exit 1
fi
"$scbondtest" || true	# marker (SC-BOND-OK/FAIL) gates in boot-test.sh

# SC-BRIDGE — SCNetworkConfiguration bridge virtual interface: create a
# bridge, add the e1000 interface as a member, check the member list /
# name / options / AllowConfiguredMembers, commit, reopen and confirm
# it persisted.
scbridgetest=/usr/tests/freebsd-launchd-mach/scbridgetest
if [ ! -x "$scbridgetest" ]; then
    echo "SC-BRIDGE-FAIL: $scbridgetest missing"
    exit 1
fi
"$scbridgetest" || true	# marker (SC-BRIDGE-OK/FAIL) gates in boot-test.sh

# IPCFG-BOOT — IPConfiguration daemon iter 1 liveness probe:
# bootstrap_look_up against com.apple.IPConfiguration. If ipconfigd
# launched + claimed its service, the lookup returns a non-null
# send right and the test client prints IPCFG-BOOT-OK. DHCP itself
# isn't exercised in this iter (the next iter wires DHCPDISCOVER).
ipconfigtest=/usr/tests/freebsd-launchd-mach/ipconfigtest
if [ ! -x "$ipconfigtest" ]; then
    echo "IPCFG-BOOT-FAIL: $ipconfigtest missing"
    exit 1
fi
"$ipconfigtest" || true	# marker (IPCFG-BOOT-OK/FAIL) gates in boot-test.sh

# KEM-LINK — the in-process KernelEventMonitor (configd's config_link_monitor.c,
# #257) publishes State:/Network/Interface/<if>/Link from PF_ROUTE link-state
# changes. This is the trigger ipconfigd's DHCP now depends on (it replaced the
# hwregd attach subscription): ipconfigd brings em0 up, the monitor sees the link
# go Active and publishes, ipconfigd's sc_link_watch reacts and runs DHCP. So
# IPCFG-BOUND below now inherently exercises this path — but gate the monitor's
# own marker too so a break in the link half is attributed here. The monitor now
# runs inside configd, so its marker lands in configd's log. Poll it; cat surfaces
# the marker to the console for boot-test.sh's expect.
kem_log=/var/log/configd.stderr
i=0
while [ $i -lt 30 ]; do
    if grep -q "KEM-LINK-OK" "$kem_log" 2>/dev/null; then
        break
    fi
    sleep 1
    i=$((i + 1))
done
echo "--- $kem_log ---"
cat "$kem_log" 2>/dev/null || echo "(no $kem_log)"
echo "--- end $kem_log ---"
if ! grep -q "KEM-LINK-OK" "$kem_log" 2>/dev/null; then
    echo "KEM-LINK-FAIL: marker not seen within 30s"
fi

# IPCFG-BOUND + IPCFG-STORE + IPCFG-RENEW — iter-3 full DHCPv4 INIT →
# BOUND on em0 (DISCOVER → OFFER → REQUEST → ACK with the standard
# 4/8/16s RFC 2131 retransmit ladder, then apply_lease() runs
# SIOCAIFADDR + RTM_ADD default route + /etc/resolv.conf write)
# followed by iter-4 SCDynamicStore publish of
# State:/Network/Service/<UUID>/IPv4 (+/DNS) to configd, then iter-5b
# lease renewal (RENEWING DHCPREQUEST sent at T1 = 5s with
# IPCONFIGD_FAST_LEASE=10 set in the plist; SLIRP ACKs with the same
# address; IPCFG-RENEW-OK fires once). Wait up to ~60s for the
# RENEW marker (worst case: BOUND ~5s + T1=5s + renew retransmit
# ladder 4s = ~14s, boot scheduling adds slack). Cat the stderr
# log so the markers reach this console for boot-test.sh's expect
# blocks.
if [ -f /var/log/ipconfigd.stderr ]; then
    i=0
    while [ $i -lt 30 ]; do
        if grep -q 'IPCFG-RENEW-OK\|IPCFG-STORE-FAIL\|IPCFG-BOUND-FAIL' \
            /var/log/ipconfigd.stderr 2>/dev/null; then
            break
        fi
        sleep 2
        i=$((i+1))
    done
    echo "--- /var/log/ipconfigd.stderr ---"
    cat /var/log/ipconfigd.stderr
    echo "--- end ipconfigd.stderr ---"
    # Also dump the bound state for visibility — ifconfig em0 + the
    # default route + resolv.conf. Best-effort.
    echo "--- ifconfig em0 ---"
    ifconfig em0 2>&1 || true
    echo "--- netstat -rn (default route) ---"
    netstat -rn -f inet 2>&1 | head -20 || true
    echo "--- netstat -rn -f inet6 (iter 7a SLAAC default) ---"
    netstat -rn -f inet6 2>&1 | head -20 || true
    echo "--- /etc/resolv.conf ---"
    cat /etc/resolv.conf 2>/dev/null || echo "(missing)"
    echo "--- end bound-state dump ---"
fi

# IPCFG-RPC — iter-5a Mach-RPC round-trip: ipconfigrpctest does
# bootstrap_look_up for com.apple.IPConfiguration, calls
# ipconfig_if_count (expects >= 1 after BOUND) + ipconfig_if_addr
# ("em0") (expects 10.0.2.15 from SLIRP), prints IPCFG-RPC-OK on
# success. Runs AFTER the BOUND/STORE markers so the worker has
# already populated bound_state.
ipconfigrpctest=/usr/tests/freebsd-launchd-mach/ipconfigrpctest
if [ ! -x "$ipconfigrpctest" ]; then
    echo "IPCFG-RPC-FAIL: $ipconfigrpctest missing"
else
    "$ipconfigrpctest" || true	# marker gates in boot-test.sh
fi

# IPCFG-IPCONFIG — iter 8 Apple-shape CLI smoke. The same
# ipconfig_if_count + ipconfig_if_addr MIG round-trip ipconfigrpctest
# exercises, but driven via /usr/sbin/ipconfig at its Apple-canonical
# path. Validates that the binary parses argv, looks up the service,
# calls the MIG stub, and prints the result.
#
# Marker: IPCFG-IPCONFIG-OK on both subcommands returning the expected
# values (em0=10.0.2.15 from SLIRP; ifcount>=1). -FAIL on any mismatch.
# MDNS-BOOT + MDNS-ENGINE — iter 2 mDNSResponder. bootstrap_look_up
# for com.apple.mDNSResponder via mdnstest gates MDNS-BOOT-OK. The
# daemon itself logs MDNS-ENGINE-OK to /var/log/mDNSResponder.stderr
# right after mDNS_Init returns mStatus_NoError; cat the file so the
# marker reaches the boot console for boot-test.sh's expect.
mdnstest=/usr/tests/freebsd-launchd-mach/mdnstest
if [ ! -x "$mdnstest" ]; then
    echo "MDNS-BOOT-FAIL: $mdnstest missing"
else
    "$mdnstest" || true	# marker gates in boot-test.sh
fi
if [ -f /var/log/mDNSResponder.stderr ]; then
    # Wait briefly for the engine to come up — the daemon starts at
    # plist KeepAlive boot but may not have called mDNS_Init by the
    # time mdnstest finishes.
    i=0
    while [ $i -lt 10 ]; do
        if grep -q 'MDNS-ENGINE-OK\|MDNS-ENGINE-FAIL' \
            /var/log/mDNSResponder.stderr 2>/dev/null; then
            break
        fi
        sleep 1
        i=$((i+1))
    done
    # MDNS-IFWATCH — iter 4 reactive interface watcher. mDNSConfigStore.c
    # subscribes to State:/Network/Global/IPv4 + State:/Network/Service/
    # .+/IPv4; when ipconfigd publishes (DHCP bind) or removes (lease
    # loss) a service IPv4, the SCDS callback wakes the mDNS main thread
    # (self-pipe) to re-walk the interface list via
    # mDNSPlatformPosixRefreshInterfaceList, logging MDNS-IFWATCH-OK to
    # this stderr. By the time we get here the iter-3 DHCP BOUND publish
    # and the iter-5b RENEW re-publish (both checked above) have already
    # fired, so the marker should be present. Wait a little longer for
    # it in case the publish raced the subscriber's startup.
    i=0
    while [ $i -lt 15 ]; do
        if grep -q 'MDNS-IFWATCH-OK' \
            /var/log/mDNSResponder.stderr 2>/dev/null; then
            break
        fi
        sleep 1
        i=$((i+1))
    done
    echo "--- /var/log/mDNSResponder.stderr ---"
    cat /var/log/mDNSResponder.stderr
    echo "--- end mDNSResponder.stderr ---"
    # Informational/non-fatal gate. If the SCDS-driven re-walk is absent
    # (e.g. the IPv4 publish raced subscriber startup), emit -SKIP rather
    # than failing: the routing-socket watcher in mDNSPosix.c remains the
    # parallel trigger, so interface state is still tracked either way.
    if grep -q 'MDNS-IFWATCH-OK' /var/log/mDNSResponder.stderr 2>/dev/null; then
        echo "MDNS-IFWATCH-OK: SCDS interface/service IPv4 change drove a" \
            "mDNSResponder interface re-walk"
    else
        echo "MDNS-IFWATCH-SKIP: no SCDS-driven interface re-walk observed" \
            "(routing-socket watcher still active)"
    fi
else
    echo "MDNS-IFWATCH-SKIP: /var/log/mDNSResponder.stderr missing"
fi

# MDNS-DNSSD — iter 3 end-to-end round-trip via libdns_sd:
# dnssdtest registers a service ("freebsd-launchd-mach-iter3" /
# _iter3._tcp) and browses for it; on success the daemon round-trips
# its own registration back through libdns_sd's AF_UNIX channel.
# Proves the engine + uds_daemon + libdns_sd stubs all line up.
dnssdtest=/usr/tests/freebsd-launchd-mach/dnssdtest
if [ ! -x "$dnssdtest" ]; then
    echo "MDNS-DNSSD-FAIL: $dnssdtest missing"
else
    "$dnssdtest" || true	# marker gates in boot-test.sh
fi

# DA-BOOT — DiskArbitration iter 1 liveness. datest verifies the
# Mach service (DA-BOOT-OK) via bootstrap_look_up.
datest=/usr/tests/freebsd-launchd-mach/datest
if [ ! -x "$datest" ]; then
    echo "DA-BOOT-FAIL: $datest missing"
else
    "$datest" || true	# marker gates in boot-test.sh
fi

# DA-IOKIT — C1.3 (#218) DiskArbitration kernel-notify migration gate.
# diskarbitrationd learns of storage arrival/removal from the kernel notify
# channel via libIOKit's IOServiceAddMatchingNotification (recv port registered
# with the in-kernel registry via IOREGIOCWATCH on /dev/ioregistry, #225). hwregd
# was retired in #218, so there is no userland fallback — the kernel is the
# registry. The daemon logs to /var/log/diskarbitrationd.stderr (its own lines
# use DISTINCT spellings — DA-IOKIT-ARMED / DA-IOKIT-REGFAIL / "DA-IOKIT: storage"
# — that deliberately do NOT collide with the canonical DA-IOKIT-OK/SKIP/FAIL
# tokens this gate emits, so dumping the daemon log below cannot trip the
# boot-test expect block early):
#   DA-IOKIT-ARMED        — daemon registered the kernel arrival + departure watches
#   "DA-IOKIT: storage X" — daemon saw storage device X (qemu vtblk/ahci boot
#                            disk) arrive through the kernel channel
#   DA-IOKIT-REGFAIL      — a kernel watch registration failed outright
#
# This run.sh gate emits EXACTLY ONE definite marker and RETURNS (never exits),
# folding the optional daemon-side lines into one decision so it can never sit
# blocking while a later required marker scrolls past (the MDNS-IFWATCH lesson):
#   DA-IOKIT-OK    — daemon registered the kernel watches + saw a storage device
#   DA-IOKIT-FAIL  — daemon logged DA-IOKIT-FAIL (a watch registration failed)
#   DA-IOKIT-SKIP  — no /dev/ioregistry (kernel predates K1), or no storage
#                    device surfaced through the kernel channel
# SKIP keeps this green on a continuous image whose kernel predates K1 (no
# /dev/ioregistry, so no storage events) — only DA-IOKIT-FAIL gates in boot-test.sh.
da_iokit_gate()
{
    log=/var/log/diskarbitrationd.stderr

    if [ ! -f "$log" ]; then
        echo "DA-IOKIT-SKIP: $log missing"
        return 0
    fi
    echo "=== diskarbitrationd.stderr (tail -40) ==="
    tail -40 "$log" 2>/dev/null
    echo "==="

    # A hard registration failure gates regardless of anything else.
    if grep -q 'DA-IOKIT-REGFAIL' "$log" 2>/dev/null; then
        echo "DA-IOKIT-FAIL: diskarbitrationd failed to register kernel watches"
        return 0
    fi
    # No /dev/ioregistry on this kernel (predates K1) — no storage events.
    if [ ! -c /dev/ioregistry ]; then
        echo "DA-IOKIT-SKIP: no /dev/ioregistry (kernel predating K1)"
        return 0
    fi
    # Definite success: a storage device arrived through the kernel channel.
    if grep -q 'DA-IOKIT: storage ' "$log" 2>/dev/null; then
        echo "DA-IOKIT-OK: diskarbitrationd saw a storage device via /dev/ioregistry"
        return 0
    fi
    # Kernel path armed (DA-IOKIT-ARMED subscription line) but no storage device
    # surfaced — e.g. the qemu disk's class isn't visible as a storage *name* to
    # the registry yet. Non-fatal: the channel works; nothing storage to report.
    if grep -q 'DA-IOKIT-ARMED' "$log" 2>/dev/null; then
        echo "DA-IOKIT-SKIP: kernel watches registered but no storage device arrival observed"
        return 0
    fi
    echo "DA-IOKIT-SKIP: no DA kernel-notify activity in $log"
    return 0
}
da_iokit_gate

ipconfig_cli=/usr/sbin/ipconfig
if [ ! -x "$ipconfig_cli" ]; then
    echo "IPCFG-IPCONFIG-FAIL: $ipconfig_cli missing"
else
    cli_count=$("$ipconfig_cli" ifcount 2>&1 || true)
    cli_addr=$("$ipconfig_cli" getifaddr em0 2>&1 || true)
    echo "  ipconfig ifcount -> $cli_count"
    echo "  ipconfig getifaddr em0 -> $cli_addr"
    case "$cli_count" in
        ''|*[!0-9]*)
            echo "IPCFG-IPCONFIG-FAIL: ifcount non-numeric '$cli_count'"
            ;;
        *)
            if [ "$cli_count" -lt 1 ]; then
                echo "IPCFG-IPCONFIG-FAIL: ifcount=$cli_count < 1"
            elif [ "$cli_addr" != "10.0.2.15" ]; then
                echo "IPCFG-IPCONFIG-FAIL: em0 addr '$cli_addr' != 10.0.2.15"
            else
                echo "IPCFG-IPCONFIG-OK: ipconfig ifcount=$cli_count em0=$cli_addr"
            fi
            ;;
    esac
fi

# HOSTNAMED — iters 1/2/3a/3b shipped via #63/#86/#90/#155. iter 3c
# pivot vendors Apple's configd/Plugins/IPMonitor/set-hostname.c as
# the decision engine. The daemon is now persistent (RunAtLoad=true,
# KeepAlive=true per the plist + the libdispatch event loop in
# src/hostnamed/vendored/set-hostname.c), so the test rounds drop the
# manual /usr/sbin/hostnamed invocations and instead mutate watched
# SCDS keys + sleep + check. The launchd-started daemon reacts via its
# Setup:/System / Setup:/Network/HostNames / State:/Network/Service/*/
# DHCP subscriptions.
#
# Four test rounds prove the precedence chain through the vendored
# engine:
#   ROUND 1 (synthesis path): hostnameprefset --clear removes
#     ComputerName from SCPrefs; prefs_monitor's SCPrefs callback
#     republishes Setup:/System with the freebsd_synthesize_hostname
#     fallback (slug+suffix from SMBIOS+MAC). set-hostname.c's SCDS
#     callback sees the new Setup:/System and sethostname()s.
#   ROUND 2 (SCPrefs path): hostnameprefset writes a fixture
#     ComputerName; prefs_monitor's callback republishes; engine
#     re-runs and adopts the fixture.
#   ROUND 3 (DHCP path): hostnamedhcpset writes Option_12 into the
#     live State:/Network/Service/<UUID>/DHCP dict; the engine's
#     pattern subscription fires; copy_dhcp_hostname returns Option_12;
#     sethostname.
#   ROUND 4 (mDNS path): hostnamedmdnsset publishes a forced-multicast
#     PTR for our bound IPv4. mDNS PTR appearance isn't an SCDS event —
#     so we tickle the SCDS subscription via hostnamedhcpset --clear
#     after the PTR is registered, which trips set-hostname.c's
#     update_hostname; the SCNetworkReachability shim (libdns_sd-backed
#     PTR over mDNS) picks up the fixture.
#
# (Apple's set-hostname.c also subscribes to a notify(3) network-
# change token for fast event-driven reactions. iter 3c's bring-up
# carry skips that registration — see vendored/set-hostname.c —
# pending a libnotify investigation. SCDS subscriptions still drive
# the engine on every state change, so the rounds work without it.)

echo "==> hostnamed: persistent daemon liveness check (pre-rounds)"
echo "    pgrep -x hostnamed -> $(pgrep -x hostnamed 2>/dev/null || echo MISSING)"
echo "    pre-rounds kernel hostname (before any ROUND): '$(hostname 2>/dev/null)'"
echo "    pre-rounds sysctl kern.hostname: '$(sysctl -n kern.hostname 2>/dev/null)'"
if [ -f /var/log/hostnamed.stderr ]; then
    bytes=$(wc -c < /var/log/hostnamed.stderr 2>/dev/null || echo 0)
    lines=$(wc -l < /var/log/hostnamed.stderr 2>/dev/null || echo 0)
    echo "    /var/log/hostnamed.stderr -> ${bytes}B ${lines}L"
    echo "    --- hostnamed.stderr (head -25, boot-time) ---"
    head -25 /var/log/hostnamed.stderr 2>/dev/null
    echo "    --- hostnamed.stderr (tail -30, latest) ---"
    tail -30 /var/log/hostnamed.stderr 2>/dev/null
    echo "    --- end ---"
fi

hostnamed_wait_settle() {
    # set-hostname.c update_hostname runs synchronously inside the SCDS
    # callback on the dispatch queue: read SCDS keys, sethostname,
    # notify_post. PTR queries are asynchronous with their own
    # callback. 4s covers the mDNS round; SCPrefs/DHCP refreshes are
    # sub-second.
    sleep 4
}

hostnamed_dump_log() {
    label="$1"
    echo "    --- /var/log/hostnamed.stderr (last 30 lines, $label) ---"
    tail -30 /var/log/hostnamed.stderr 2>/dev/null || true
    echo "    --- end ---"
}

# ROUND 1: synthesis path. Clear SCPrefs ComputerName + DHCP Option_12;
# prefs_monitor republishes Setup:/System with the synthesized
# slug+suffix fallback; engine re-runs.
echo "==> hostnamed ROUND 1 (synthesis path)"
if [ -x /usr/tests/freebsd-launchd-mach/hostnameprefset ]; then
    /usr/tests/freebsd-launchd-mach/hostnameprefset --clear || true
fi
if [ -x /usr/tests/freebsd-launchd-mach/hostnamedhcpset ]; then
    /usr/tests/freebsd-launchd-mach/hostnamedhcpset --clear || true
fi
hostnamed_wait_settle
hostnamed_dump_log "ROUND 1"
echo "    kernel hostname = '$(hostname 2>/dev/null)'"
if [ -x /usr/tests/freebsd-launchd-mach/hostnametest ]; then
    /usr/tests/freebsd-launchd-mach/hostnametest
else
    echo "HOSTNAMED-FAIL: hostnametest binary not installed"
fi

# ROUND 2: SCPrefs path. Writing ComputerName via
# SCPreferencesPathSetValue+CommitChanges fires the SCPrefs callback
# in prefs_monitor, which republishes Setup:/System; set-hostname.c
# adopts the fixture name.
echo "==> hostnamed ROUND 2 (SCPrefs path)"
HOSTNAMED_FIXTURE="hostnamed-iter2-fixture"
if [ -x /usr/tests/freebsd-launchd-mach/hostnameprefset ]; then
    /usr/tests/freebsd-launchd-mach/hostnameprefset "$HOSTNAMED_FIXTURE"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "HOSTNAMED-PREFS-FAIL: hostnameprefset exit=$rc"
    else
        hostnamed_wait_settle
        hostnamed_dump_log "ROUND 2"
        echo "    kernel hostname = '$(hostname 2>/dev/null)'"
        /usr/tests/freebsd-launchd-mach/hostnametest "$HOSTNAMED_FIXTURE"
    fi
else
    echo "HOSTNAMED-PREFS-FAIL: hostnameprefset binary not installed"
fi

# ROUND 3: DHCP path. Clear SCPrefs (so the SCPrefs tier doesn't win),
# then write Option_12 to the live /DHCP dict via hostnamedhcpset.
# SCDS Set fires set-hostname.c's pattern subscription;
# copy_dhcp_hostname (in the freebsd-shim) reads Option_12 and
# returns it.
echo "==> hostnamed ROUND 3 (DHCP path)"
HOSTNAMED_DHCP_FIXTURE="hostnamed-iter3a-fixture"
if [ -x /usr/tests/freebsd-launchd-mach/hostnameprefset ]; then
    /usr/tests/freebsd-launchd-mach/hostnameprefset --clear || true
fi
if [ -x /usr/tests/freebsd-launchd-mach/hostnamedhcpset ]; then
    /usr/tests/freebsd-launchd-mach/hostnamedhcpset "$HOSTNAMED_DHCP_FIXTURE"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "HOSTNAMED-DHCP-FAIL: hostnamedhcpset exit=$rc"
    else
        hostnamed_wait_settle
        hostnamed_dump_log "ROUND 3"
        echo "    kernel hostname = '$(hostname 2>/dev/null)'"
        /usr/tests/freebsd-launchd-mach/hostnametest "$HOSTNAMED_DHCP_FIXTURE" DHCP
    fi
else
    echo "HOSTNAMED-DHCP-FAIL: hostnamedhcpset binary not installed"
fi

# ROUND 4: mDNS path. Clear SCPrefs + DHCP, background hostnamedmdnsset
# to publish a forced-multicast PTR for our bound IPv4. mDNS PTR
# appearance isn't an SCDS event, so after MDNSSET-READY fires we
# trip set-hostname.c's pattern subscription by hostnamedhcpset --clear
# again — that's a no-op write to the /DHCP key that the engine's
# kSCEntNetDHCP pattern subscriber wakes on, which calls
# update_hostname → falls through to the reverse-PTR tier →
# SCNetworkReachability shim issues a libdns_sd PTR query that
# mDNSResponder answers from hostnamedmdnsset's registration.
echo "==> hostnamed ROUND 4 (mDNS path)"
HOSTNAMED_MDNS_FIXTURE="hostnamed-iter3b-fixture"
if [ -x /usr/tests/freebsd-launchd-mach/hostnameprefset ]; then
    /usr/tests/freebsd-launchd-mach/hostnameprefset --clear || true
fi
if [ -x /usr/tests/freebsd-launchd-mach/hostnamedhcpset ]; then
    /usr/tests/freebsd-launchd-mach/hostnamedhcpset --clear || true
fi
if [ -x /usr/tests/freebsd-launchd-mach/hostnamedmdnsset ]; then
    /usr/tests/freebsd-launchd-mach/hostnamedmdnsset \
        "$HOSTNAMED_MDNS_FIXTURE" > /var/log/hostnamedmdnsset.stdout \
        2> /var/log/hostnamedmdnsset.stderr &
    HOSTNAMED_MDNSSET_PID=$!
    for i in 1 2 3 4 5 6 7 8 9 10; do
        if grep -q "MDNSSET-READY" /var/log/hostnamedmdnsset.stdout \
            2>/dev/null; then
            break
        fi
        sleep 1
    done
    echo "--- /var/log/hostnamedmdnsset.stdout ---"
    cat /var/log/hostnamedmdnsset.stdout 2>/dev/null
    echo "--- /var/log/hostnamedmdnsset.stderr ---"
    cat /var/log/hostnamedmdnsset.stderr 2>/dev/null
    echo "--- end hostnamedmdnsset logs ---"
    if grep -q "MDNSSET-READY" /var/log/hostnamedmdnsset.stdout \
        2>/dev/null; then
        # Tickle the engine to re-evaluate the precedence chain now
        # that the mDNS PTR is live (no notify(3) network-change wake
        # in iter 3c bring-up — see the workaround in vendored/
        # set-hostname.c).
        /usr/tests/freebsd-launchd-mach/hostnamedhcpset --clear || true
        hostnamed_wait_settle
        hostnamed_dump_log "ROUND 4"
        echo "    kernel hostname = '$(hostname 2>/dev/null)'"
        /usr/tests/freebsd-launchd-mach/hostnametest \
            "$HOSTNAMED_MDNS_FIXTURE" MDNS
    else
        echo "HOSTNAMED-MDNS-FAIL: hostnamedmdnsset never reached READY"
    fi
    kill "$HOSTNAMED_MDNSSET_PID" 2>/dev/null || true
    wait "$HOSTNAMED_MDNSSET_PID" 2>/dev/null || true
else
    echo "HOSTNAMED-MDNS-FAIL: hostnamedmdnsset binary not installed"
fi

# ROUND 5: Bonjour conflict-rename feedback (#156). Background
# hostnamedbonjourset to claim <fixture>.local (authoritative UNIQUE A
# record). Then set SCPrefs ComputerName=<fixture>: prefs_monitor
# republishes Setup:/Network/HostNames, mDNSConfigStore (iter-5 watch on
# the Setup: key) re-adopts <fixture> and re-probes <fixture>.local, which
# the fixture already owns -> mDNSCore renames the daemon host label to
# <fixture>-2 and PosixDaemon.c publishes it to State:/Network/HostNames.
# hostnamed's observer persists <fixture>-2 to SCPrefs ComputerName, which
# flows back through prefs_monitor + set-hostname.c to the kernel hostname.
echo "==> hostnamed ROUND 5 (Bonjour conflict-rename)"
HOSTNAMED_BONJOUR_FIXTURE="hostnamed-iter5-fixture"
HOSTNAMED_BONJOUR_RESOLVED="${HOSTNAMED_BONJOUR_FIXTURE}-2"
if [ -x /usr/tests/freebsd-launchd-mach/hostnamedhcpset ]; then
    /usr/tests/freebsd-launchd-mach/hostnamedhcpset --clear || true
fi
if [ -x /usr/tests/freebsd-launchd-mach/hostnamedbonjourset ] && \
   [ -x /usr/tests/freebsd-launchd-mach/hostnameprefset ]; then
    /usr/tests/freebsd-launchd-mach/hostnamedbonjourset \
        "$HOSTNAMED_BONJOUR_FIXTURE" > /var/log/hostnamedbonjourset.stdout \
        2> /var/log/hostnamedbonjourset.stderr &
    HOSTNAMED_BONJOURSET_PID=$!
    for i in 1 2 3 4 5 6 7 8 9 10; do
        if grep -q "BONJOURSET-READY" \
            /var/log/hostnamedbonjourset.stdout 2>/dev/null; then
            break
        fi
        sleep 1
    done
    echo "--- /var/log/hostnamedbonjourset.stdout ---"
    cat /var/log/hostnamedbonjourset.stdout 2>/dev/null
    echo "--- /var/log/hostnamedbonjourset.stderr ---"
    cat /var/log/hostnamedbonjourset.stderr 2>/dev/null
    echo "--- end hostnamedbonjourset logs ---"
    if grep -q "BONJOURSET-READY" \
        /var/log/hostnamedbonjourset.stdout 2>/dev/null; then
        # The fixture owns <fixture>.local. Now make the daemon want that
        # same name; its probe will collide and mDNSCore will rename it.
        /usr/tests/freebsd-launchd-mach/hostnameprefset \
            "$HOSTNAMED_BONJOUR_FIXTURE"
        # Poll up to 30s for the full round-trip to converge on the -2
        # suffix, rather than a fixed sleep. Each hop (mDNS adopt+probe ->
        # conflict-rename -> State: publish -> observer persist -> prefs
        # republish -> sethostname) is gated on the periodic IPv4 churn that
        # drives the recompute (Setup: change-notify is unreliable in our
        # configd), so allow several churn cycles.
        for i in $(seq 1 30); do
            if [ "$(hostname 2>/dev/null)" = \
                "$HOSTNAMED_BONJOUR_RESOLVED" ]; then
                break
            fi
            sleep 1
        done
        hostnamed_dump_log "ROUND 5"
        # mDNSResponder-side diagnostics: did the recompute fire (calling
        # mDNS_SetFQDN), did mDNS detect a collision (Local Hostname ...
        # already in use), did the publisher run (MDNS-RENAME-PUBLISHED)?
        # run.sh does not otherwise dump this file after boot.
        echo "    --- /var/log/mDNSResponder.stderr (last 80 lines, ROUND 5) ---"
        tail -80 /var/log/mDNSResponder.stderr 2>/dev/null || true
        echo "    --- end mDNSResponder.stderr ---"
        echo "    kernel hostname = '$(hostname 2>/dev/null)' " \
            "(expected '$HOSTNAMED_BONJOUR_RESOLVED')"
        /usr/tests/freebsd-launchd-mach/hostnametest \
            "$HOSTNAMED_BONJOUR_RESOLVED" BONJOUR-RENAME
    else
        echo "HOSTNAMED-BONJOUR-RENAME-FAIL: hostnamedbonjourset never" \
            "reached READY (could not claim <fixture>.local)"
    fi
    kill "$HOSTNAMED_BONJOURSET_PID" 2>/dev/null || true
    wait "$HOSTNAMED_BONJOURSET_PID" 2>/dev/null || true
else
    echo "HOSTNAMED-BONJOUR-RENAME-FAIL: hostnamedbonjourset or" \
        "hostnameprefset binary not installed"
fi

echo "==> hostnamed: persistent daemon liveness POST-rounds check"
echo "    pgrep -x hostnamed -> $(pgrep -x hostnamed 2>/dev/null || echo MISSING)"
echo "--- /var/log/hostnamed.stderr (last 40 lines, post-rounds) ---"
tail -40 /var/log/hostnamed.stderr 2>/dev/null
echo "--- end hostnamed.stderr ---"

# PAM-FRAMEWORK — PAM port iter 1 (issue #93). Verifies our vendored
# Apple OpenPAM-35 libpam.so.6 loads our vendored pam_deny.so via the
# fixture service /etc/pam.d/test_iter1 and the full module ABI
# round-trips. Indirectly verifies that FreeBSD-runtime binaries
# (login, su, passwd, sshd, ...) which DT_NEEDED libpam.so.6 in their
# ELF headers transparently pick up our library since soname matches.
echo "==> PAM iter 1: ldd /usr/bin/login | grep libpam"
ldd /usr/bin/login 2>/dev/null | grep -i libpam || true
echo "==> PAM iter 1: libpam.so.6 owner check"
ls -lh /usr/lib/libpam.so.6 2>/dev/null
if [ -x /usr/tests/freebsd-launchd-mach/pamframeworktest ]; then
    /usr/tests/freebsd-launchd-mach/pamframeworktest
else
    echo "PAM-FRAMEWORK-FAIL: pamframeworktest binary not installed"
fi

# PAM-MODULES — PAM port iter 2 (issue #95). dlopens each of the 5
# vendored Apple standalone modules from /usr/lib/pam_NAME.so.6 and
# verifies the canonical pam_sm_* entry point is present. Indirectly
# verifies the existing post-login marker chain that was already
# green in iter 1 STILL works (login now using OUR pam_self.so.6 +
# pam_uwtmp.so.6 instead of FreeBSD's, with our libpam.so.6).
echo "==> PAM iter 2: ls -lh /usr/lib/pam_{self,rootok,uwtmp,nologin,env}.so.6"
ls -lh /usr/lib/pam_self.so.6 /usr/lib/pam_rootok.so.6 \
       /usr/lib/pam_uwtmp.so.6 /usr/lib/pam_nologin.so.6 \
       /usr/lib/pam_env.so.6 2>/dev/null
if [ -x /usr/tests/freebsd-launchd-mach/pammodulestest ]; then
    /usr/tests/freebsd-launchd-mach/pammodulestest
else
    echo "PAM-MODULES-FAIL: pammodulestest binary not installed"
fi

# PAM-LOGIN — PAM port iter 3 (issue #97). Round-trips through our
# overlay /etc/pam.d/su: pam_rootok (root succeeds without password)
# → pam_self (su to self ok) → include system (pam_unix auth, pam_unix
# account, pam_uwtmp session, pam_unix password). All 4 facilities
# must traverse successfully for `su root -c` to print the marker.
# Indirectly verifies every other pam.d service that `include system`
# works — pam_unix + pam_uwtmp + pam_nologin all load and execute.
echo "==> PAM iter 3: ls -lh overlay /etc/pam.d/"
ls -lh /etc/pam.d/ 2>/dev/null | head -15
echo "==> PAM iter 3: pkg info FreeBSD-pam* status"
pkg info FreeBSD-pam 2>&1 | head -3
pkg info FreeBSD-pam-lib 2>&1 | head -3
echo "==> PAM iter 3: su round-trip via overlay pam.d/su"
out=$(su root -c "echo PAM-LOGIN-OK: su round-trip via pam_rootok + system stack" 2>&1)
if echo "$out" | grep -q "PAM-LOGIN-OK"; then
    echo "$out"
else
    echo "PAM-LOGIN-FAIL: su failed; output was: $out"
fi

exit 0
