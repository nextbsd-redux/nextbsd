# mach_kmod (historical — source moved)

> **The Mach kernel source has MOVED.** mach is now compiled **into** the
> NEXTBSD kernel (`options COMPAT_MACH`, #181); the canonical source lives in
> the **`nextbsd-kernel`** repo at `src-overlay/sys/compat/mach/`. The
> out-of-tree `.ko` build and its duplicated source here were removed (#189,
> #190). **Do not edit mach kernel code here — there is none anymore.**
>
> What remains in this directory: `tests/` (userland mach syscall tests, built
> and run by the boot smoke test), `smoke-sysctls.sh` (a runtime sysctl
> regression script), and the historical notes below.

Originally: an out-of-tree FreeBSD kernel module providing Mach IPC primitives
(`mach_msg`, `mach_port_*`, port-rights transfer, no-senders /
dead-name notifications) — the foundation that lets Apple-source
daemons like configd, notifyd, asl, and (eventually) Apple's launchd
run on FreeBSD.

## Source lineage

| Tree                       | Source                                                            | License                        |
|---------------------------|-------------------------------------------------------------------|--------------------------------|
| `src/`                    | ravynOS legacy FreeBSD branch `sys/compat/mach/`                  | OSF / CMU / BSD 2-Clause       |
| `include/sys/mach/`       | ravynOS legacy FreeBSD branch `sys/sys/mach/`                     | OSF / CMU / BSD 2-Clause       |
| `include/apple/`          | ravynOS `include/apple/` (Apple-ism userland-shape headers)       | per-file (mostly Apple APSL)   |
| `opt_*.h`                 | Hand-written; match FreeBSD GENERIC kernel config                 | BSD-2-Clause (this project)    |
| `Makefile`                | Hand-written from ravynOS `sys/modules/mach/Makefile`             | BSD-2-Clause (this project)    |

ravynOS's `sys/compat/mach/` is itself derived from NextBSD's port of
XNU's `osfmk/ipc/`. Per-file copyright headers are preserved verbatim.

## Build prerequisites

- FreeBSD host (or build VM) with `/usr/src/sys/` available. Install
  `src.txz` from the matching FreeBSD release if missing.
- `make`, `clang`, `ld` — all in base.
- No additional packages.

End users **consuming** the pre-built `mach.ko` tarball from this repo's
GitHub releases do not need `/usr/src/sys/` — only people building from
source do.

## Default build (GENERIC kernel)

```sh
make
sudo make install   # installs to /boot/kernel/mach.ko
```

This is what CI does. The shipped `opt_compat_mach.h`, `opt_capsicum.h`,
and `opt_ntp.h` match a stock GENERIC kernel config, so the resulting
`mach.ko` works on any GENERIC-built FreeBSD kernel within the same
major release.

## Custom kernel build (KERNBUILDDIR override)

If you're running a custom kernel — any config that differs from
GENERIC on `CAPABILITIES` or `PPS_SYNC`, or any future option that
ends up gated by a Mach `#ifdef` — rebuild against your kernel's
actual build directory:

```sh
make KERNBUILDDIR=/usr/obj/usr/src/sys/MYKERNEL
sudo make install
```

`bsd.kmod.mk` reads `opt_*.h` from `${KERNBUILDDIR}/` when that variable
is set, overriding the GENERIC-matching defaults shipped here. No
source changes, no `#ifdef __FreeBSD_version` wallpaper anywhere in
the module itself.

## What each `opt_*.h` declares

| File                       | Default value         | Why                                                                  |
|---------------------------|-----------------------|----------------------------------------------------------------------|
| `opt_compat_mach.h`       | `#define COMPAT_MACH 1` | Module-local. Always on. Not influenced by KERNBUILDDIR.            |
| `opt_capsicum.h`          | `#define CAPABILITIES 1` | Matches GENERIC. Affects Mach IPC's cred-handling paths.            |
| `opt_ntp.h`               | empty                  | Matches GENERIC. PPS_SYNC is off; Mach clock code respects.          |

## Loading at boot

The build pipeline copies `mach.ko` to `/boot/kernel/mach.ko` on the live
ISO and adds `mach_load="YES"` to `/boot/loader.conf` so loader preloads
it before `init` runs. On an installed system, the standard approach is
either:

- `/boot/loader.conf`: `mach_load="YES"` (loader preload, available
  in kernel-only context even before root mount)
- `/etc/rc.conf`: `kld_list="mach"` (loaded by `/etc/rc.d/kld` early in
  multi-user boot, after root mount)

Use the first form for any kernel-only consumers (none today, but
future kernel-side users will want it preloaded). Use the second form
for purely userland consumers — slightly later in boot, but easier to
unload/reload during development.

## Kernel ABI / version compatibility

FreeBSD's KBI policy: stable within a major release, may break across
majors. This module is built and tested against:

- FreeBSD 15.x

For other majors, rebuild from source. The same C is intended to work
unchanged; only the matrix entry in CI needs to expand. No
`#if __FreeBSD_version` shims in the module sources.

## Userland API

`mach.ko` registers syscalls via `syscall_helper_register` and an
`EVFILT_MACHPORT` kqueue filter. Userspace consumers (libmach, libxpc,
launchd, configd) call into the module via those syscalls and through
`/dev/mach` for control operations. The userland libraries that wrap
these primitives are not part of this module; they live in
`src/libmach/`, `src/libxpc/`, etc. (post-checkpoint scope).
