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

# 1. kernel-side: mach.ko loaded.
if kldstat -m mach >/dev/null 2>&1; then
    echo "MACH-SMOKE-OK: mach.ko is loaded"
    kldstat -v 2>/dev/null | grep -i mach || true
else
    echo "MACH-SMOKE-FAIL: mach.ko is NOT loaded"
    echo "kldstat output:"
    kldstat
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
# Mach IPC backend test (DISPATCH_SOURCE_TYPE_MACH_RECV) lands in a
# follow-up commit once event_mach_freebsd.c is wired in.
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

# 4. Mach IPC backend round-trip: DISPATCH_SOURCE_TYPE_MACH_RECV with
# the real polling-thread backend (event_mach_freebsd.c) — allocate a
# port via mach_reply_port, attach a dispatch source, self-send a
# message, verify the handler fires within 5s and consumes it. Proves:
# event_mach_freebsd.c spawns a working poll thread; mach_msg(MACH_RCV_
# LARGE, rcv_size=0) peek path returns TOO_LARGE without consuming;
# dispatch_source_merge_data wakes the handler; handler's mach_msg(
# MACH_RCV_MSG) drains the message; clean cancel/release teardown.
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

# 3. Runtime invocation deferred. The mach.ko null-port-send patch
#    (commit 0e380f6) DID land and IS active — no more
#    "ipc_entry_lookup failed on 0" kernel print — but launchctl
#    still hangs after the initial vproc_swap_integer call, likely
#    on a follow-on Mach receive that's uninterruptible (D-state
#    sleep). Even `timeout 10 /bin/launchctl help` blocks the
#    shell pipeline forever because SIGKILL can't reap a process
#    stuck in uninterruptible kernel sleep, and the `$()` capture
#    waits for the child to exit.
#
#    Full launchctl runtime smoke needs at minimum:
#      - launchd-as-PID-1 to provide a valid bootstrap_port, OR
#      - additional mach.ko patches to make Mach receive
#        interruptible-via-signal on init paths
#
#    Tracked in memory: mach-msg-send-to-null-port-hangs.

echo "LAUNCHCTL-BUILD-OK: /bin/launchctl exists ($(stat -f%z /bin/launchctl) bytes), all libsystem deps resolve"

# 10. ASL runtime smoke (Phase J). Task #41 move_member wire-up
# landed but a follow-on halt-after-bootstrap-remote regression is
# under investigation. Keep test at SKIP for now.
sleep 2

# Task #39 debugging: each daemon plist redirects stderr to its own
# /var/log/<daemon>.stderr file. Dump those into the boot console
# BEFORE the proc check so [T39-bs] / [T39-ll] traces (and any other
# diagnostic output) survive the halt that follows a PROC-FAIL exit.
for slog in /var/log/syslogd.stderr /var/log/notifyd.stderr /var/log/hwregd.stderr /var/log/aslmanager.stderr /var/log/configd.stderr; do
    if [ -s "$slog" ]; then
        echo "=== begin $slog ==="
        cat "$slog" || true
        echo "=== end $slog ==="
    fi
done

if pgrep -x notifyd >/dev/null 2>&1; then
    echo "NOTIFYD-PROC-OK: notifyd running as pid $(pgrep -x notifyd)"
else
    echo "NOTIFYD-PROC-FAIL: notifyd not running"
    ps auxww | grep -E 'syslogd|notifyd' || true
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

# HWREG-PUBSUB — hwregd Mach pub/sub round-trip: subscribe to the
# org.freebsd.hwregd service and confirm the subscription ack event.
hwregtest=/usr/tests/freebsd-launchd-mach/hwregtest
if [ ! -x "$hwregtest" ]; then
    echo "HWREG-PUBSUB-FAIL: $hwregtest missing"
    exit 1
fi
"$hwregtest" || true	# marker (HWREG-PUBSUB-OK/FAIL) gates in boot-test.sh

# HWREG-RPC — hwregd Mach-RPC query API: walk the registry tree via
# the MIG hwreg.defs routines (get_root / get_children / get_node).
hwregquery=/usr/tests/freebsd-launchd-mach/hwregquery
if [ ! -x "$hwregquery" ]; then
    echo "HWREG-RPC-FAIL: $hwregquery missing"
    exit 1
fi
"$hwregquery" || true	# marker (HWREG-RPC-OK/FAIL) gates in boot-test.sh

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

# IOKIT-WALK — libIOKit iter 1 read-only registry walk: walk the
# hwregd registry tree through the IOKit facade (IORegistryGetRoot
# Entry / GetChildIterator / IOIteratorNext / GetName / GetPath),
# exercise IOObject Retain+Release.
iokittest=/usr/tests/freebsd-launchd-mach/iokittest
if [ ! -x "$iokittest" ]; then
    echo "IOKIT-WALK-FAIL: $iokittest missing"
    exit 1
fi
"$iokittest" || true	# marker (IOKIT-WALK-OK/FAIL) gates in boot-test.sh

# IOKIT-MATCH — libIOKit iter 2 properties + matching: pull a node's
# property bag as a CFDictionary, fetch single CFString/CFNumber
# properties, exercise IOServiceMatching + IOServiceGetMatching
# Service(s) against PCIDevice (deterministic via PCI enrichment).
iokitmatchtest=/usr/tests/freebsd-launchd-mach/iokitmatchtest
if [ ! -x "$iokitmatchtest" ]; then
    echo "IOKIT-MATCH-FAIL: $iokitmatchtest missing"
    exit 1
fi
"$iokitmatchtest" || true	# marker (IOKIT-MATCH-OK/FAIL) gates in boot-test.sh

# IOKIT-IOREG — libIOKit iter 3 ioreg(8) tool: the K1 success marker
# in the IOKit-userland port plan ("ioreg -l works"). Runs the
# installed /usr/sbin/ioreg with -l (tree + property bags) and with
# -c CPU (class filter) and validates the output is non-trivial and
# contains the expected class header line. Marker is emitted by this
# shell, not by a C test binary — ioreg itself is the thing under
# test.
ioreg=/usr/sbin/ioreg
if [ ! -x "$ioreg" ]; then
    echo "IOKIT-IOREG-FAIL: $ioreg missing"
    exit 1
fi
ioreg_log=/tmp/ioreg.out
if ! "$ioreg" -l > "$ioreg_log" 2>&1; then
    echo "IOKIT-IOREG-FAIL: ioreg -l exited non-zero"
elif ! lines=$(wc -l < "$ioreg_log") || [ "$lines" -lt 20 ]; then
    echo "IOKIT-IOREG-FAIL: ioreg -l produced too little output (lines=$lines)"
elif ! "$ioreg" -c CPU 2>&1 | grep -q '<class CPU>'; then
    echo "IOKIT-IOREG-FAIL: ioreg -c CPU did not surface a CPU node"
else
    echo "IOKIT-IOREG-OK: ioreg -l works (lines=$lines), -c CPU finds CPU nodes"
fi

# IOKIT-NOTIFY — libIOKit iter 4 K2 device notifications: allocate
# an IONotificationPort, SetDispatchQueue, AddMatchingNotification
# for PCIDevice (initial-arming iterator should hand back ≥1 entry
# from the QEMU PCI bus), then a no-match class (initial-arming
# iterator empty but non-NULL), tear it down. Async device-arrival
# fire isn't injectable from this CI; the underlying raw-mach_msg
# receive-thread pattern is structurally identical to HWREG-PUBSUB
# / SC-NOTIFY, both already CI-proven.
iokitnotifytest=/usr/tests/freebsd-launchd-mach/iokitnotifytest
if [ ! -x "$iokitnotifytest" ]; then
    echo "IOKIT-NOTIFY-FAIL: $iokitnotifytest missing"
    exit 1
fi
"$iokitnotifytest" || true	# marker (IOKIT-NOTIFY-OK/FAIL) gates in boot-test.sh

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
    echo "--- /var/log/mDNSResponder.stderr ---"
    cat /var/log/mDNSResponder.stderr
    echo "--- end mDNSResponder.stderr ---"
fi

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

exit 0
