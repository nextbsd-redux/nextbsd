# freebsd-launchd-mach

FreeBSD modernization project. The plan: bring Apple's better lower-level
system services (Mach IPC, launchd, libxpc, libdispatch, configd,
notifyd, asl, IPConfiguration, mDNSResponder, DiskArbitration) to
FreeBSD as an out-of-tree kernel module + userland stack. FreeBSD ELF
for the binary format throughout. macOS compatibility is a bonus; the
goal is a more modern FreeBSD.

**Scope:** this image ships the *system-services* layer only &mdash;
`mach.ko` + libsystem_kernel + libdispatch + libxpc + liblaunch +
launchd + libCoreFoundation (swift-corelibs) + the daemons built on
top. **GNUstep is not on this ISO** &mdash; it's the framework/app
layer that a separate gershwin desktop overlay (forked from this
base) handles. Foundation/`NSXxx` consumers live in that overlay,
not here. The `CoreFoundation` we ship is swift-corelibs-foundation's
pure-C CF, used by `launchctl` / `configd` / `IPConfiguration` /
`mDNSResponder` / etc. &mdash; all of which are pure-C-with-CF in
Apple's upstream and remain so on FreeBSD. See the
[launchctl CoreFoundation spike](https://pkgdemon.github.io/freebsd-launchctl-corefoundation-spike.html)
for the full reasoning behind that choice.

- **Plan:** <https://pkgdemon.github.io/freebsd-launchd-mach-plan.html>
- **License:** [BSD-2-Clause](LICENSE) (with per-component Apache 2.0 /
  LGPL / MIT / OSF / CMU headers preserved on imported files &mdash;
  see [NOTICE](NOTICE))
- **Companion:** the [`freebsd-launchd`](https://github.com/pkgdemon/freebsd-launchd)
  repo, the AF_UNIX-only track. This repo is the pure-port track.

## Status

**Phase A** &mdash; *done.* ISO build pipeline lifted from
[`freebsd-launchd`](https://github.com/pkgdemon/freebsd-launchd) works in
CI; live ISO boots stock FreeBSD `init` as PID 1. Curated pkgbase install
(no full `base.txz`/`kernel.txz` extraction) keeps the rootfs lean.

**Phase B** &mdash; *done.* ravynOS's `sys/compat/mach/` ported as an
out-of-tree `mach.ko`. Build gate and runtime gate both met: module
compiles, packages, installs to `/boot/kernel/mach.ko`, and kldloads
cleanly on stock FreeBSD-15.0-RELEASE-p8. Survives fork/exec/exit
stress traffic. ~400 lines of out-of-tree compat shim covering eight
kernel-side deltas (p_machdata aliasing, EVFILT_MACHPORT soft-fail,
Apple syscall arg structs, etc.). Documented at
[`src/mach_kmod/PHASE_B_FINDINGS.md`](src/mach_kmod/PHASE_B_FINDINGS.md).

**Phase B Tier 1** &mdash; *done.* Userland-callable surface added on top
of the kmod:

- **4 wired Mach syscalls** (no-arg trap family): `mach_reply_port`
  (210), `task_self_trap` (211), `thread_self_trap` (212),
  `host_self_trap` (213). Dynamically allocated via
  `kern_syscall_register`; numbers published via
  `sysctl mach.syscall.<name>`.
- **2 live stats sysctls**: `mach.stats.ports_in_use`,
  `mach.stats.kmsgs_in_use` (uma_zone_get_cur over IPC zones).
- **2 in-kernel test sysctls**: `mach.test_port_lifecycle` (port
  alloc/dealloc stress), `mach.test_in_kernel_mqueue` (kmsg/mqueue
  primitive exercise without going through the high-level
  send/receive routing).
- 16-check regression smoke at `src/mach_kmod/smoke-sysctls.sh`.

**Phase C1, C2, C3** &mdash; *done.* Userland Mach bridge:

- **C1: `libmach`** &mdash; userland shared library at
  [`src/libmach/`](src/libmach/) that exposes the wired syscalls as
  named C functions (`mach_reply_port()`, `mach_task_self()`, etc.) and
  `mach_msg()`. Resolves syscall numbers from sysctl on first call.
- **C2: lazy Mach init** &mdash; `mach_task_init_lazy()` and
  `mach_thread_init_lazy()` in the kmod give pre-load processes real
  Mach state on first wired-syscall call instead of `MACH_PORT_NULL`.
- **C3: end-to-end `mach_msg`** &mdash; wired `mach_msg_trap` (syscall
  214) and verified from userland: a test program allocates a reply
  port via libmach, calls `mach_msg(MACH_RCV_MSG | MACH_RCV_TIMEOUT,
  timeout=0)`, kernel returns `MACH_RCV_TIMED_OUT` (0x10004003) as
  expected. Real Mach IPC reachable from userland through libmach.

Total smoke at this checkpoint: **19/19 green** on stock FreeBSD-15
with zero kernel patches.

**Phase D** &mdash; *pkgbase + libsystem layout* &mdash; *done.* Pivoted
away from gershwin-developer's `/System/Library/Libraries/` for our
system libraries. Apple-canonical `libsystem_*` layout under
`/usr/lib/system/` instead, with the
[install layout spike](https://pkgdemon.github.io/freebsd-libxpc-install-layout-spike.html)
as the design doc:

- **Pkgbase migration.** ISO base now installed via curated
  `FreeBSD-*` pkgbase set (`pkglist-base.txt` + `buildpkgs-base.txt`)
  rather than `base.txz`/`kernel.txz` extraction. Lets us omit
  kerberos / lldb / dtrace / audit (runtime) / hyperv-tools /
  bsdinstall / etc. that base.txz had no opt-out for.
- **`libmach` &rarr; `libsystem_kernel`.** Renamed and relocated to
  `/usr/lib/system/libsystem_kernel.so` with `libsystem_kernel.pc`
  pkg-config and headers at `/usr/include/mach/`. `build.sh`
  primes `/var/run/ld-elf.so.hints` inside `rootfs.uzip` via
  `chroot $rootfs ldconfig -m /usr/lib /usr/lib/system` so the
  runtime linker resolves `/usr/lib/system/` libraries without an
  `/etc/ld-elf.so.conf` entry &mdash; nothing under `/usr/local`.
- **`swift-corelibs-libdispatch` vendored** under
  [`src/libdispatch/`](src/libdispatch/), built in our `build.sh`
  chroot pipeline (cmake/ninja). gershwin-developer's FreeBSD
  performance patch (`event_kevent.c` timer fixes +
  `workqueue.c` loop-var typo) applied as a commit on the vendored
  tree. Installs as `/usr/lib/system/libdispatch.so`.
- **`Block.h` + `libBlocksRuntime.so`** ship from libdispatch's bundled
  BlocksRuntime sources (Apple compiler-rt &mdash; same upstream as the
  former `FreeBSD-libblocksruntime` pkg, which is dropped). Single
  source of truth on the system, owned by us.
- **`libsystem_dispatch.so` / `libsystem_blocks.so`** symlinks for
  Apple-canonical link names (`-lsystem_dispatch`).
- **Mach source-type stubs** &mdash; `src/libdispatch/src/event/event_mach_freebsd.c`
  defines `_dispatch_source_type_mach_send/recv` plus the internal
  `_dispatch_mach_type_*` and `_dispatch_xpc_type_sigterm` symbols
  (link-level only; `dispatch_source_create` with a `MACH_*` type
  returns NULL safely). MACH_RECV gets a real polling-thread
  implementation in Phase E; SEND and the internal types stay as
  link-only stubs.
- **CI smoke** at this checkpoint: 4/4 markers green on the live ISO
  &mdash; `MACH-SMOKE-OK`, `LIBSYSTEM-KERNEL-OK`, `LIBDISPATCH-OK`,
  `LIBDISPATCH-MACH-OK`.

**Phase E** &mdash; *real `DISPATCH_SOURCE_TYPE_MACH_RECV` backend* &mdash;
*done.* The stub `event_mach_freebsd.c` is now a working polling-thread
implementation. `dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV,
port, 0, q)` returns a live source backed by:

- A per-source detached `pthread` that loops on `mach_msg(MACH_RCV_MSG |
  MACH_RCV_LARGE | MACH_RCV_TIMEOUT, rcv_size=0, port, 100ms)`. The
  `MACH_RCV_LARGE` + `rcv_size=0` trick gets `MACH_RCV_TOO_LARGE`
  back from the kernel *without* consuming the message (ravynOS's
  `ipc_mqueue.c` honors the "don't take it off the queue" branch on
  the `LARGE` option). The polling thread fires
  `dispatch_source_merge_data(ds, 1)` on the no-msg&rarr;msg edge so
  libdispatch schedules the user's event handler; the handler reads
  the message itself via `mach_msg(MACH_RCV_MSG, ...)`. Apple's
  documented contract for `MACH_RECV` sources.
- Lifecycle: the thread `_dispatch_retain`s the source as soon as
  `du_owner_wref` is populated, which pins the unote (source dispose
  is what frees the `dispatch_source_refs_s`). The thread exits when
  `dispatch_source_testcancel(ds)` becomes true, then
  `_dispatch_release`s &mdash; breaking the retain so the source / unote
  dealloc normally. No kernel patches; no `HAVE_MACH`; no
  `EVFILT_MACHPORG`.
- libsystem_kernel header (`mach/message.h`) now ships `MACH_RCV_LARGE`,
  `MACH_RCV_TOO_LARGE`, `MACH_MSG_TYPE_MAKE_SEND_ONCE`, and the
  `MACH_MSGH_BITS()` packing macro &mdash; the userland surface needed
  to construct send-once self-messages and run the new round-trip test.
- `test_libdispatch_mach` upgraded from stub-link check to a true
  round-trip: allocates a receive port via `mach_reply_port`, attaches
  a `MACH_RECV` source, sends a self-message via
  `MACH_MSG_TYPE_MAKE_SEND_ONCE`, asserts the handler fires within 5
  seconds and `msgh_id` survives the kernel hop. Same `LIBDISPATCH-MACH-OK`
  CI marker, stronger meaning.
- CI smoke at this checkpoint: still 4/4 markers green on the live ISO.

**Phase F (port-management syscalls)** &mdash; *done.* Three new
Mach traps wired into mach.ko + exposed through libsystem_kernel,
prerequisites for the ravynOS-fork libxpc and the bootstrap server:

- `mach_port_allocate(task, right, &name)` — allocate a fresh port
  (RECEIVE / SEND / PORT_SET / DEAD_NAME). Syscall 215 on amd64.
- `mach_port_deallocate(task, name)` — drop a send / send-once
  right from the task's namespace. Syscall 216.
- `mach_port_insert_right(task, name, poly, polyPoly)` — attach a
  port right onto an existing name (typically MAKE_SEND on a
  receive-right port; the path libxpc uses for service endpoints).
  Syscall 217.
- All three handlers (`sys__kernelrpc_mach_port_*_trap`) and arg
  structs (`_kernelrpc_mach_port_*_trap_args`) were already
  imported from ravynOS — just needed the
  `mach_syscall_wire.c` registration. NULL-guard wrappers lazy-init
  Mach task state and return `KERN_RESOURCE_SHORTAGE` if init fails.
- New header `mach/mach_port.h` ships `kern_return_t`,
  `mach_port_right_t`, `mach_msg_type_name_t`, and the
  `MACH_PORT_RIGHT_*` / `MACH_MSG_TYPE_*` / `KERN_*` constants
  libxpc uses.
- New smoke test `test_mach_port` does a full allocate &rarr;
  insert_right(MAKE_SEND) &rarr; self-mach_msg(SEND) &rarr;
  mach_msg(RCV) &rarr; deallocate round-trip. CI marker
  `MACH-PORT-OK` between `LIBSYSTEM-KERNEL-OK` and `LIBDISPATCH-OK`.
  5/5 markers green.

**Phase G prereqs (`task_get_special_port` / `task_set_special_port`)**
&mdash; *done.* Two more wired Mach traps that let userland publish
and retrieve `TASK_BOOTSTRAP_PORT` directly (Apple delivers these
over MIG; we expose as syscalls since we don't ship MIG). New
header `mach/task_special_ports.h` with the Apple-canonical
`task_get_bootstrap_port` / `task_set_bootstrap_port` macros. Smoke
marker: `TASK-SPECIAL-PORT-OK`.

**Phase G1 (bootstrap protocol &mdash; single task)** &mdash; *done.*
Hand-rolled message-ID dispatch over Mach IPC (no MIG):

- `bootstrap_check_in(bp, name, *port)` / `bootstrap_look_up(bp,
  name, *port)` matching libxpc's call signatures, declared at
  `/usr/include/servers/bootstrap.h`.
- `bootstrap_server_run(service_port, *stop)` server loop. Receives
  with a 500ms cancel-poll timeout, dispatches on `msgh_id`,
  replies using `MOVE_SEND_ONCE` to the request's `msgh_remote_port`
  (the kernel-manufactured send-once right from the client's
  `MAKE_SEND_ONCE`).
- Service map is a 64-slot static array in the server's address
  space &mdash; same-task scope only. Cross-process scope (Phase G2)
  lifts this to a real daemon + complex-message port-descriptor
  transfer.
- Smoke marker `BOOTSTRAP-OK`. Test allocates a service port,
  publishes it via `task_set_bootstrap_port`, spawns a pthread
  running `bootstrap_server_run`, calls `bootstrap_check_in` then
  `bootstrap_look_up` for the same name from the main thread, asserts
  the returned port names match.

**Phase G2a (complex Mach messages on the bootstrap wire)** &mdash;
*done.* Reply upgraded to a complex message
(`MACH_MSGH_BITS_COMPLEX` + `mach_msg_body_t` +
`mach_msg_port_descriptor_t` + result) so the kernel translates
the port reference between sender and receiver IPC spaces &mdash;
prerequisite for cross-task port-right transfer. Bug hunt during
validation exposed and fixed a Phase G1 false positive where the
combined `MACH_SEND_MSG | MACH_RCV_MSG` form reused the buffer for
both directions: the kernel wrote the reply into the same memory
that held the request, and the test was vacuously comparing
`0 == 0`. Now uses split `mach_msg(SEND)` + `mach_msg(RCV)` and
asserts non-`MACH_PORT_NULL` ports in the test.

**Phase G2b (host-bootstrap-port fallback)** &mdash; *done.* mach.ko
gained a HOST_BOOTSTRAP_PORT slot in `realhost.special[]`;
`task_get_special_port(TASK_BOOTSTRAP_PORT)` falls back to it when
the per-task `itk_bootstrap` slot is null. Bootstrap server calls
`host_set_special_port(HOST_BOOTSTRAP_PORT, port)` once at startup
to publish its port host-wide; every other process discovers it via
`task_get_bootstrap_port`. Smoke marker: `HOST-BOOTSTRAP-OK`.

This phase also forced an architectural pivot that affects everything
going forward:

**Mach trap multiplexer** &mdash; *done.* FreeBSD's `syscalls.master`
reserves exactly 10 dynamic syscall slots (`lkmnosys` at 210-219).
We&rsquo;ve used all 10 on foundational Mach traps; the
launchd+configd+notifyd+asl port wants 20-30 more. FreeBSD&rsquo;s
`RESERVED` slots (~48 of them) *cannot* be claimed via
`kern_syscall_register` &mdash; the kernel only accepts slots whose
`sy_call` is `lkmnosys` or `lkmressys`, and RESERVED slots use plain
`nosys`. So we built a multiplexer dispatcher:

- One FreeBSD syscall slot (219) hosts `sys_mach_trap_mux_trap`,
  which switches on an op-number argument and dispatches to the
  underlying Mach trap handler internally. Architecturally
  identical to Apple&rsquo;s `mach_trap_table` &mdash; just routed
  at the C handler level instead of in the assembly trampoline.
- Op-numbers defined in `<mach/mach_traps_mux.h>`:
  - `1` &nbsp;`host_set_special_port`
  - `2` &nbsp;`task_set_special_port` (migrated off its former
    dedicated slot 219 to free it for the multiplexer)
- Future Mach traps (`mach_port_mod_refs`,
  `mach_port_request_notification`, `mach_port_get_attributes`,
  `task_threads`, the rest of the `mach_port_*member` family,
  `vm_*`, `semaphore_*`, etc.) drop in as new op-numbers without
  consuming any further FreeBSD syscall slots. The 9 hot dedicated
  slots stay put because either they're called frequently
  (`mach_msg_trap`, `mach_reply_port`, the `_self_trap` family) or
  ABI-constrained (`mach_msg_trap` has a 7-arg shape that exceeds
  the multiplexer&rsquo;s 5-arg budget).
- Per-call overhead is ~5ns of dispatch (switch + function call) &mdash;
  comfortably under 1% of trap-entry cost. Apple&rsquo;s
  `mach_trap_table` validates the architectural pattern at scale.

Audit plan documenting the trade space:
[freebsd-mach-kmod-syscall-slots-spike](https://pkgdemon.github.io/freebsd-mach-kmod-syscall-slots-spike.html).

**Phase G2c + G2d (standalone daemon + cross-process)** &mdash;
*done.* `/usr/sbin/bootstrap_server` is a real binary: at
startup it allocates a service port, inserts a `MAKE_SEND` right,
publishes via `host_set_special_port(HOST_BOOTSTRAP_PORT)`, and
enters `bootstrap_server_run`. `run.sh` backgrounds the daemon
before running `test_bootstrap_remote` (a fresh process whose
per-task `itk_bootstrap` is null, so `task_get_bootstrap_port`
falls back to the host slot the daemon populated). The test
exercises `bootstrap_check_in` then `bootstrap_look_up` of the
same service name across IPC spaces &mdash; the cross-task
port-right transfer Phase G2a's complex-message wire encoding
made possible. Asserts non-`MACH_PORT_NULL` returns and that
both calls yield the same port name in the receiver's IPC space.
Smoke marker: `BOOTSTRAP-REMOTE-OK`. The standalone daemon is
*not* wired into `rc.local`; with launchd now PID 1 (Phase I1) +
the Path A MIG bootstrap chain landing in Phase J, this binary
remains as a harness-only artifact and a fallback test path
exercising `host_set_special_port`.

**Phase H (libxpc port)** &mdash; *done.* Forked ravynOS's
`lib/libxpc/` into `src/libxpc/`, built against our libsystem_kernel
+ libdispatch + libbootstrap. Type-system surface
(`xpc_dictionary_create`, `set_string`, `get_string`, `set_int64`,
`get_int64`, `xpc_release`) round-trips in-process; smoke marker
`LIBXPC-OK` fires alongside the prior 19 markers. Prerequisites that
shipped first:

| Capability | Phase | Marker |
|---|---|---|
| Mach IPC (`mach_msg` send/recv) | C3 | `LIBSYSTEM-KERNEL-OK` |
| `DISPATCH_SOURCE_TYPE_MACH_RECV` | E | `LIBDISPATCH-MACH-OK` |
| Port management (`mach_port_*`) | F | `MACH_PORT_OK` |
| `task_*_special_port` | G prereq | `TASK-SPECIAL-PORT-OK` |
| `host_set_special_port` + fallback | G2b | `HOST-BOOTSTRAP-OK` |
| `bootstrap_check_in` / `bootstrap_look_up` + daemon | G1/G2c/G2d | `BOOTSTRAP-OK`, `BOOTSTRAP-REMOTE-OK` |

Local Apple-shim public headers (`<Availability.h>`, `<launch.h>`,
`<uuid/uuid.h>`, `<sys/fileport.h>`) ship into `/usr/include` so
external consumers (test binaries today, future launchd / configd)
parse `<xpc/xpc.h>` without changes. Two XNU-only symbols
(`fileport_make{port,fd}`, `vproc_transaction_{begin,end}`) are
runtime stubs in `src/libxpc/stubs.c`; replace with real
implementations when a real consumer needs them. libxpc.so links
`-lsbuf` and `-lBlocksRuntime` so its DT_NEEDED entries cover the
sbuf API and `-fblocks` runtime helpers it depends on.

The high-level `dispatch_mach_t` channel API in libdispatch's
`src/mach.c` (~3,250 LOC, gated on `HAVE_MACH`) is *not* on the
critical path &mdash; ravynOS libxpc deliberately reaches for the
lower-level primitives we now have. The channel API only comes back
if we later want to support Apple-source consumers that use it
directly (e.g., specific Apple frameworks); none of our current
Phase F-G targets do.

**Phase I0 (MIG &mdash; `bootstrap_cmds`)** &mdash; *done.* Vendored
Apple's `bootstrap_cmds-138` into `src/bootstrap_cmds/` and got the
Mach Interface Generator building on FreeBSD: `migcom` (the compiler
backend) at `/usr/libexec/migcom`, `mig` (the wrapper) at
`/usr/bin/mig`. The wrapper script was rewritten from bash to POSIX
`/bin/sh` (FreeBSD base ships no bash) and de-Xcode'd (no `xcrun`, no
`-arch`). `byacc` + `flex` for the `parser.y` / `lexxer.l` grammars
come from `FreeBSD-toolchain`. Smoke marker: `MIG-BUILD-OK`. MIG is
the prerequisite for the launchd-842 port &mdash; launchd ships seven
`.defs` files whose Mach-RPC stubs are code-generated.

**Phase I1 (launchd-842 port)** &mdash; *done.* Vendored
Apple's last open-source launchd (`launchd-842.92.1`,
`apple-oss-distributions/launchd`) into `src/launchd/` &mdash; `src/`
+ `liblaunch/` + `support/`, ~27k LOC; `SystemStarter/` skipped.
Sub-phases:

- **I1a &mdash; MIG stub generation** &mdash; *done.* Vendored the
  MIG type-system (`std_types.defs` / `mach_types.defs` /
  `machine_types.defs`) into `src/libmach/include/mach/`; `build.sh`
  runs `mig` over launchd's `job` / `job_forward` / `job_reply` /
  `internal` / `helper` `.defs` files, producing 15 generated `.c`/
  `.h` stubs. `protocol_jobmgr.defs` is excluded &mdash; it's dead
  code, not in Apple's Xcode build.
- **I1b &mdash; `liblaunch`** &mdash; *done.* `liblaunch.so.1` builds
  and installs at `/usr/lib/system/`. The bulk of the work was a
  header gauntlet: ~14 new `<mach/*>` headers in `libmach`
  (`boolean.h`, `kern_return.h`, `std_types.h`, `ndr.h`, `machine.h`,
  `vm_map.h`, `clock_types.h`, `port.h`, `notify.h`, `mach_types.h`,
  `mig_errors.h`, `mig.h`, `thread_status.h`, `mach_error.h`) plus 8
  Apple-private shims in `src/launchd/freebsd-shims/`
  (`TargetConditionals.h`, `AvailabilityMacros.h`, `libkern/OS*.h`,
  `os/assumes.h`, `libproc.h`, `quarantine.h`, `System/sys/fileport.h`,
  `malloc/malloc.h`). `<uuid/uuid.h>` was rewritten as a real
  libuuid-API shim (array `uuid_t` + `uuid_generate` / `uuid_unparse`
  / &hellip;), with libxpc's `libnv` + `xpc_misc.c` / `xpc_type.c`
  updated to match.
- **I1c &mdash; `launchd` daemon binary** &mdash; *done.* `/sbin/launchd`
  (235 KiB ELF) builds, links, installs. `core.c` alone is 12k LOC;
  getting it to compile + link took ~17 new shims in
  `src/launchd/freebsd-shims/`, ~8 new `<mach/*>` headers in
  `libmach`, the launchd-internal XPC vocabulary in
  `<xpc/private.h>`, and ~25 libsystem_kernel stubs in
  `libmach/mach_traps.c` so the link succeeds and runtime calls
  fail closed. Smoke marker: `LAUNCHD-BUILD-OK` (`run.sh:262` execs
  `/sbin/launchd` on the no-IPC PID-check path and asserts it
  rejects non-PID-1 with the expected diagnostic).
- **I1d &mdash; `libCoreFoundation` + `lib_FoundationICU`** &mdash;
  *done.* Vendored swift-corelibs CoreFoundation as
  `src/libCoreFoundation/` (78 `.c` files, ~915 `CF*` symbols),
  built standalone with `DEPLOYMENT_RUNTIME_SWIFT=0` (skips the
  `libswiftCore.so` dep). Apple's slimmed ICU fork
  (`lib_FoundationICU`) vendored alongside, since CF's plist
  parser pulls in ICU headers via the private namespace. Both
  install under `/usr/lib/system/`
  (`libCoreFoundation.so.6` + `lib_FoundationICU.so`). Smoke
  marker: `COREFOUNDATION-OK` (CF + plist round-trip on the
  ISO). Rationale &mdash; why swift-corelibs CF over GNUstep
  `libs-corebase` &mdash; in the next section.
- **I1e &mdash; `launchctl`** &mdash; *done.* `/bin/launchctl`
  builds against `libCoreFoundation` and links via
  `/usr/lib/system/`. Smoke marker: `LAUNCHCTL-BUILD-OK`. The
  49th CF call missing from swift-corelibs was swapped for a
  two-line `CFReadStream` + `CFPropertyListCreateFromStream`
  equivalent, per the
  [launchctl-corefoundation-spike](https://pkgdemon.github.io/freebsd-launchctl-corefoundation-spike.html).

`launchd` is PID 1 on the running image. Boot path:
`loader -> kernel -> /sbin/launchd as PID 1`, directly &mdash; no
cd9660/uzip, no unionfs, no `init.sh` pivot, no rc.d. The kernel
mounts `/` read-only as always; launchd remounts it read-write
itself (fsck + `mount -uw /`) before the daemon scan, then starts
launchd-managed daemons via plists in
`/System/Library/LaunchDaemons/`. See the
[launchd-842 porting plan](https://pkgdemon.github.io/freebsd-launchd-842-porting-plan.html).

**Phase J (launchd-managed daemons port)** &mdash; *done.*
Once launchd was PID 1, the next bring-up was the daemons it
manages: Apple Syslog (ASL) + libnotify, plus `notifyd` /
`syslogd` / `aslmanager`. All build green and run under launchd
on the booted image.

- **J0 &mdash; vendor ASL + libnotify** &mdash; *done.* Apple's
  `Libnotify-279.40.4` and `syslog-450.10` imported into
  `src/Libnotify/` and `src/syslog/`.
- **J1 &mdash; `libnotify` client lib** &mdash; *done.* Builds
  and installs. Build marker: `NOTIFY-LIB-OK`.
- **J2 &mdash; `notifyd` + `syslogd` daemon binaries** &mdash;
  *done.* Both build and install via `src/Libnotify/notifyd/`
  and `src/syslog/syslogd.tproj/`. Apple's static ASL lib
  (`libsystem_asl.a`) builds and links into both. Build
  markers: `ASL-LIB-OK`, `NOTIFYD-BUILD-OK`,
  `SYSLOGD-BUILD-OK`, `SYSLOG-CLI-BUILD-OK`.
- **J4 &mdash; `aslmanager`** &mdash; *done.* Apple's log
  rotator builds with `copyfile` + `setattrlist` runtime
  stubs. Build marker: `ASLMANAGER-BUILD-OK`.
- **Path A &mdash; real Mach-IPC bootstrap chain (task #39)**
  &mdash; *in progress.* The hand-rolled BST bootstrap protocol
  (used by the Phase G2 tests) is being replaced by Apple's
  real MIG path so daemons can call `bootstrap_check_in`
  against launchd PID 1. Four pieces shipped:
  1. Kernel fork eventhandler in `mach.ko`
     (`src/mach_kmod/src/kern/task.c:1234`,
     `mach_task_fork_bsport`) propagates the parent task's
     `itk_bootstrap` send right to the child at fork time
     &mdash; lazy thread init preserved, since full task-init
     panicked under Phase C2.
  2. liblaunch's `.so`-load constructor
     (`src/launchd/liblaunch/libbootstrap.c:101`) populates the
     userspace `bootstrap_port` via `task_get_special_port` so
     daemons see launchd's port immediately at startup.
  3. libxpc migrated off the BST hand-rolled protocol; the
     production path is now Apple-canonical MIG via
     `liblaunch.so` (commit `119bf44`). libxpc, libnotify and
     notifyd link `-llaunch`.
  4. `set_security_token` is called in `mach_task_fork_bsport`
     and `mach_task_init_lazy` (commit `701cf90`) so the
     child's `audit_token.val[5]` carries its real PID &mdash;
     without this, the MIG trailer reaches launchd with PID=0
     and `job_mig_intran` can't find the calling job, so
     `job_mig_check_in2` sees `j=NULL` and returns
     `BOOTSTRAP_NO_MEMORY`.

  Path A is green: `notifyd` and `syslogd` both run under
  launchd (`NOTIFYD-PROC-OK`, `SYSLOGD-PROC-OK`), and the
  end-to-end ASL round-trip passes &mdash; `SYSLOG-RUN-OK`
  confirms a message posted via `syslog(3)` to `/var/run/log`
  is routed to `/var/log/system.log`.

- **J5 &mdash; runtime validation** &mdash; *done.* The full
  boot test is green in CI: launchd PID 1 &rarr; daemons &rarr;
  login, with the Mach-IPC syslog round-trip verified. The
  livecd (cd9660 + uzip + unionfs) was replaced with a bootable
  GPT disk image (BIOS + UEFI, read-write UFS root), and
  launchd PID 1 now remounts `/` read-write itself before the
  daemon scan.

  *Known debt (task #41):* syslogd's clean boot currently
  depends on skipping kevent-backed libdispatch dispatch
  sources &mdash; `dispatch_resume()` of a VNODE/SIGNAL/READ
  source deadlocks in `mach_msg_send()` under the `HAVE_MACH`
  libdispatch (the dispatch manager never drains its port).
  syslogd works around it (VNODE file-monitors off,
  `klog_in`/`udp_in` disabled, SIGHUP source dropped); fixing
  libdispatch is tracked separately.

**Phase K (hardware registry + IOKit userland)** &mdash; *in
progress.* FreeBSD-devd and FreeBSD-devmatch are not in the
runtime image, so hot-plug `kldload`, device-arrival and
link-state events have no provider. `hwregd` &mdash; a native
hardware-registry daemon with an IOKit-shaped Mach-RPC API
&mdash; closes that gap, and Apple's `IOKitUser` will sit on top
as a thin facade (re-aimed at `hwregd` rather than a kernel
IOService graph &mdash; there is no IOKit kernel here). hwregd
Phase 0 (skeleton daemon, plist, `/dev/devctl` reader, Mach pub/sub)
and Phase 1 (the structured registry + a 10-routine Mach-RPC
query/watch/load API + PCI enrichment) have landed; the launchd
`HardwareMatch` key has begun. `configd` &mdash; Apple's
SystemConfiguration daemon, hosting the `SCDynamicStore` key/value
store &mdash; has a feature-complete server over Mach IPC, and the
CF-typed `SystemConfiguration` client framework on top of it
(`libSystemConfiguration`: the full `SCDynamicStore` client API and the
`SCPreferences` persistent-configuration API) is complete. Apple's
`SCNetworkConfiguration` (the network-service / interface model), the
IOKitUser facade (`ioreg`) and `IPConfiguration` (network config, the
proper replacement for the interim `dhclient` setup) follow.
Full design: the
[hardware registry + IOKit porting plan](https://pkgdemon.github.io/freebsd-hardware-registry-iokit-plan.html).

### Phase K &mdash; configd + libSystemConfiguration (SCDynamicStore + SCPreferences) complete (2026-05-22)

**hwregd** is a working hardware-registry daemon. **Phase 0** (the
`/dev/devctl` event reader, `kldload`-on-nomatch, and a Mach pub/sub
bus) and **Phase 1** (the structured registry) have both landed:

- a tree of `hw_node` records, one per newbus device, built at startup
  from the `hw.bus.*` sysctls (the data `libdevinfo` exposes, read
  directly so the daemon needs no `FreeBSD-devmatch` dependency) and
  kept current by `/dev/devctl` attach/detach events;
- a 10-routine MIG Mach-RPC API &mdash; `hwreg_get_root` /
  `_get_children` / `_get_node` / `_get_properties` / `_lookup`
  (query), `_watch` / `_unwatch` (criteria-filtered device-event
  watch), `_load_driver`, `_retain` / `_release`;
- PCI device-identity enrichment (vendor / device / subvendor / class,
  via `/dev/pci` + `PCIOCGETCONF`).

**launchd `HardwareMatch`** &mdash; a plist key that fires a job on a
hwregd device event &mdash; has begun: the plist-key parsing has
landed, and the hwregd-subscription wiring is implemented.

**configd** &mdash; Apple's SystemConfiguration daemon, hosting the
**SCDynamicStore** key/value store over a MIG Mach protocol &mdash;
has a feature-complete server. It is ported on the *Mach-IPC track*
(real Mach IPC + MIG `config.defs`), not the sockets /
Distributed-Objects model of the companion `freebsd-launchd` repo.
Landed across seven iterations:

- the configd daemon &mdash; checks its
  `com.apple.SystemConfiguration.configd` Mach service in and runs a
  raw `mach_msg` MIG demux loop;
- the SCDynamicStore key/value store &mdash; `configopen` /
  `configget` / `configset` / `configremove` (`CONFIGD-STORE` CI
  marker);
- per-session Mach ports and change notifications &mdash; each client
  gets its own session port (joined to a port set, torn down by a
  no-senders notification when the client exits); `notifyadd` /
  `notifyviaport` / `notifychanges` watch keys, register a
  notification port and drain the changed-key list (`CONFIGD-NOTIFY`);
- POSIX-regex pattern watches &mdash; a session can watch a regex, not
  just an explicit key (`CONFIGD-PATTERN`);
- key listing &mdash; `configlist` by prefix or regex (`CONFIGD-LIST`);
- batch routines &mdash; `notifyset` (replace a whole watch set),
  `configget_m` / `configset_m` (multi-key fetch and update)
  (`CONFIGD-MULTI`).

Each is exercised end-to-end over Mach IPC by a dedicated CI test
client; `configd --selftest` additionally runs the whole protocol
in-process.

`config.defs` carries its payloads in inline bounded arrays rather
than Apple's out-of-line Mach data: cross-process out-of-line copyout
is broken in this kernel (the `mach_vm_allocate` ANYWHERE allocator
hands the receiver colliding low addresses), so configd and hwregd
both use the inline-array workaround. The cost is an 8&nbsp;KiB cap on
a key or value.

**libSystemConfiguration** &mdash; the CF-typed `SystemConfiguration`
client framework &mdash; sits on top of configd, so real software links
`-lSystemConfiguration` instead of hand-writing `config.defs` MIG. It
installs `/usr/lib/system/libSystemConfiguration.so` and the public
headers at `/usr/include/SystemConfiguration/`, and is complete across
two API families:

- the **SCDynamicStore** client API &mdash; `SCDynamicStoreCreate` plus
  `CopyValue` / `SetValue` / `AddValue` / `RemoveValue` / `CopyKeyList`,
  change notifications delivered on a dispatch queue
  (`SCDynamicStoreSetDispatchQueue`) or a run loop
  (`SCDynamicStoreCreateRunLoopSource`), and the batch `CopyMultiple` /
  `SetMultiple` (`SC-STORE` / `SC-NOTIFY` / `SC-RUNLOOP` / `SC-MULTI`
  CI markers);
- the **SCPreferences** API &mdash; the persistent-configuration plist:
  `SCPreferencesCreate` plus `GetValue` / `SetValue` / `RemoveValue` /
  `CopyKeyList` / `CommitChanges`, the `/`-separated path accessors
  (`SCPreferencesPathGetValue` / &hellip;), the exclusive
  `SCPreferencesLock`, and commit notifications
  (`SCPreferencesSetCallback`) (`SC-PREFS` / `SC-PATH` / `SC-LOCK` /
  `SC-PNOTIFY`).

The `SCDynamicStore` object is a CoreFoundation runtime type; values
cross the wire as XML property lists. Apple builds the run-loop source
and the notification delivery on `CFMachPort`, which this repo's
CoreFoundation does not compile &mdash; and libdispatch's
`MACH_RECV` sources do not reliably deliver here (the task&nbsp;#41
limitation `hwregd` also hit) &mdash; so both run on a dedicated
`mach_msg` receive thread instead. Every API is exercised end-to-end
against the live configd by a dedicated CI test client.

**Next:** Apple's `SCNetworkConfiguration` (the network-service /
interface / protocol model, built on the `SCPreferences` path
accessors); then the IOKitUser facade (`ioreg`) and `IPConfiguration`.
Deferred: USB / thermal property enrichment in hwregd; lifting
configd's 8&nbsp;KiB cap (needs the kernel's out-of-line allocator
fixed); configd's `notifyviafd` and `snapshot` routines (no consumer /
fileport support needed). Build/test runs on the `bsd01` VirtualBox VM
(interactive serial console reachable via the `joe-windows` host).

## CoreFoundation for system services &mdash; swift-corelibs CF

The launchd daemon itself uses **zero** CoreFoundation (audit
2026-05-15: 0 `CF*` calls across `launchd.c` + `core.c` +
`runtime.c` + `ipc.c` + `log.c` + `kill2.c` + `ktrace.c`). It reads
plists through the lower-level `launch_data_t` API in `liblaunch`.
Any consumer that talks to launchd over Mach RPC &mdash; test
programs, eventually `configd`, third-party daemons &mdash; can do
so without a CF implementation.

`launchctl` is the first CF-using binary on the ISO. After
auditing three candidate CoreFoundation implementations
(`gnustep/libs-base`, `gnustep/libs-corebase`,
`swiftlang/swift-corelibs-foundation` &mdash; see the
[launchctl-corefoundation-spike](https://pkgdemon.github.io/freebsd-launchctl-corefoundation-spike.html)
for the full evidence), the answer landed on swift-corelibs CF:

- **`libs-base`** contributes zero CF C surface (it's Foundation
  NSXxx for Obj-C consumers). Not relevant to a pure-C CF client.
- **`libs-corebase`** has the right shape (909 CF functions
  defined) but its plist parser is stubbed: XML read returns NULL,
  binary read is `#if 0`'d, binary write has an empty body. Fatal
  for `launchctl`, which exists to load `.plist` job files.
- **`swift-corelibs-foundation`**'s `Sources/CoreFoundation/`
  ships 78 `.c` files with 915 distinct `CF*` function definitions
  and a real Apple plist driver (XML + binary, read + write). Covers
  48 of 49 functions `launchctl` calls (the 49th is replaced by a
  two-line `CFReadStream` + `CFPropertyListCreateFromStream` swap).
  Apache 2.0, actively maintained.

Shipped as `src/libCoreFoundation/`, built with
`DEPLOYMENT_RUNTIME_SWIFT=0` (skips the libswiftCore.so dep),
installed as `/usr/lib/system/libCoreFoundation.so.6` &mdash;
same install lane as liblaunch / libxpc / libdispatch /
libsystem_kernel / libBlocksRuntime, the Apple-libsystem family
this project ships. See Phase I1d above.

**GNUstep is not on this ISO.** Per the
[launchctl spike](https://pkgdemon.github.io/freebsd-launchctl-corefoundation-spike.html),
none of the launchd-adjacent system services (launchctl, configd,
IPConfiguration, mDNSResponder, asl, notifyd, DiskArbitration) need
Foundation/NS &mdash; they're all pure C with CF, by Apple's
original design. libobjc2 + libgnustep-base + libgnustep-corebase
remain on the broader roadmap for the gershwin desktop
overlay/fork, on a parallel track that doesn't gate or block this
ISO's system-services work.

The companion
[`freebsd-libxpc-foundation-spike`](https://pkgdemon.github.io/freebsd-libxpc-foundation-spike.html)
is being rewritten to match this conclusion; the
[launchctl-corefoundation-spike](https://pkgdemon.github.io/freebsd-launchctl-corefoundation-spike.html)
is the current authoritative source while that update lands.

Plan docs:

- [**`freebsd-libxpc-plan`**](https://pkgdemon.github.io/freebsd-libxpc-plan.html)
  &mdash; phased porting plan for libxpc on top of mach.ko. Phase 0 was
  the libdispatch Mach backend; subsequent phases fork libxpc from
  ravynOS's tree, add a bootstrap server, swap freebsd-launchd's daemon
  for a clean Apple `launchd-842.92.1` import, build a hybrid
  CoreFoundation library, and finally bring up configd. Also audits the
  related daemons (asl, notifyd, IPConfiguration, mDNSResponder).
- [**`freebsd-libxpc-install-layout-spike`**](https://pkgdemon.github.io/freebsd-libxpc-install-layout-spike.html)
  &mdash; per-component install-path decisions across the four
  candidate layouts (Apple macOS reference, `/usr/lib/system/`,
  gershwin `/System/Library/`, FreeBSD-base `/usr/lib`). Drives the
  Phase D layout above.
- [**`freebsd-libxpc-libdispatch-mach-spike`**](https://pkgdemon.github.io/freebsd-libxpc-libdispatch-mach-spike.html)
  &mdash; libdispatch Mach-backend design + vendoring + build
  integration story.
- [**`freebsd-launchctl-corefoundation-spike`**](https://pkgdemon.github.io/freebsd-launchctl-corefoundation-spike.html)
  &mdash; *current authoritative answer for the CoreFoundation question
  on this ISO.* Audits launchctl's CF surface (49 functions + 1 SPI),
  compares the three CF candidates, recommends swift-corelibs CoreFoundation
  built with `DEPLOYMENT_RUNTIME_SWIFT=0`. Covers coexistence with
  GNUstep + future Swift on the ISO (three CF lanes, one CF per binary).
- [**`freebsd-libxpc-foundation-spike`**](https://pkgdemon.github.io/freebsd-libxpc-foundation-spike.html)
  &mdash; older Foundation spike, written before the launchctl audit.
  Being rewritten to match the launchctl-corefoundation-spike's
  conclusion (swift-corelibs CF for system services, GNUstep for the
  gershwin desktop overlay only). The newer spike supersedes its
  recommendations.

## Build

CI builds inside a FreeBSD VM via `vmactions/freebsd-vm@v1` and publishes
a continuous release on every push to `main` after the build + boot
smoke-test pass.

To build locally on FreeBSD:

```sh
sh build.sh
```

Produces `out/disk.img.gz` &mdash; a gzip-compressed bootable GPT
disk image (BIOS + UEFI, read-write UFS root).

## Releases

Every push to `main` that passes build + boot test is published as a
dated continuous release &mdash; the gzip-compressed GPT disk image.
