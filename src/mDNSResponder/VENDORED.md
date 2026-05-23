# Vendored Apple sources — mDNSResponder

The `mDNSCore/`, `mDNSPosix/`, `mDNSShared/`, and `DSO/` subdirs were
imported verbatim from the upstream Apple
[apple-oss-distributions/mDNSResponder](https://github.com/apple-oss-distributions/mDNSResponder)
release used by this repo (`/Users/jmaloney/Documents/launchd/mDNSResponder/`).
`LICENSE` is the upstream license file (Apache 2.0 + BSD-3-Clause hybrid
per Apple convention).

## Subdir map

| Subdir | Origin | Purpose |
|---|---|---|
| `mDNSCore/` | upstream `mDNSCore/` | Platform-agnostic mDNS protocol engine: `mDNS.c`, `DNSDigest.c`, `uDNS.c`, `DNSCommon.c`. Zero Mach surface — pure RFC 6762/6763 + DNS message parsing. |
| `mDNSPosix/` | upstream `mDNSPosix/` | POSIX platform binding: `mDNSPosix.c` (kqueue + sockets), `mDNSUNP.c` (UNP helpers), `PosixDaemon.c` (the daemon's `main`). Targets Linux/FreeBSD/NetBSD/Solaris uniformly. |
| `mDNSShared/` | upstream `mDNSShared/` | Cross-cut shared code: `uds_daemon.c` (Unix Domain Socket server speaking the dns-sd wire protocol), `dnssd_ipc.c`, `ClientRequests.c`, `dnssd_clientshim.c`, `mDNSDebug.c`, `GenLinkedList.c`, `PlatformCommon.c`, `mdns_addr_tailq.c`, `misc_utilities.c`. |
| `DSO/` | upstream `DSO/` | DNS Stateful Operations (RFC 8490) transport: `dso.c`, `dso-transport.c`. Needed by some uDNS push-update paths. |

Skipped from upstream (not needed for the iter-2 daemon): `mDNSMacOSX/`
(Apple-tied; IOKit + KeychainServices + SystemConfiguration + powerd),
`mDNSWindows/`, `Clients/`, `ServiceRegistration/`, `mDNSResponder.proj/`.
`Documents/` and most build-system files at the top of upstream are also
out — we drive the build via our own `Makefile` here.

## Local edits

### `mDNSPosix/PosixDaemon.c` — Mach service hook

Two lines added to `main()` so the daemon claims the
`com.apple.mDNSResponder` Mach service (via `bootstrap_check_in`) at
startup, and one log line after `mDNS_Init` succeeds (so the boot test
can gate on `MDNS-ENGINE-OK`). The patch is at the top of `main` plus
one line after the engine init; the rest of PosixDaemon.c is unchanged.
See `mach_bridge.c` for the bridge implementation.

This is the only edit to vendored source in iter 2.

## Why not just build upstream's Makefile?

Upstream's `mDNSPosix/Makefile` is a GNU make file targeting
Linux/Solaris/NetBSD/etc. with its own debug-vs-release, TLS
configuration, package-build pipeline, and SystemV/upstart init script
installer. We have our own bsd.prog.mk-shaped Makefile that compiles
just the daemon objects (no TLS, no nss_mdns, no Java/Bonjour clients)
and installs to our rootfs at `/usr/sbin/mDNSResponder`. The
`DAEMONOBJS` source list at upstream `Makefile:254-259` is the
authoritative file inventory we mirror.
