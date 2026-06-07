# freebsd-launchd-mach

A FreeBSD where the low-level system plumbing has been quietly
swapped out for the macOS-style equivalents: **Apple's `launchd`
runs as PID 1**, services are described by `.plist` files, hardware
events flow through a Mach-IPC bus, and the network stack is
configured by Apple's `IPConfiguration` daemon talking to Apple's
`configd`. The whole thing is still FreeBSD &mdash; same kernel,
same ELF binaries, same `pkg(8)`, same packages &mdash; but with
the modern parts of macOS's service model lifted over.

> **Project name update in flight.** The current repo name reflects
> this project's origin; a rebrand to either **NextBSD** or
> **machstep** is queued (issue #78). The technical content here is
> the same either way.

If you just want to try it, jump to [Try it in 5 minutes](#try-it-in-5-minutes).
If you want the long technical answer to *what got ported and how*,
see [PORTING.md](PORTING.md).

## What's different from stock FreeBSD

| Stock FreeBSD | This image |
|---|---|
| `init(8)` is PID 1 | **`launchd(8)`** is PID 1 |
| Services configured in `/etc/rc.conf` + `rc.d/*` scripts | Services configured in **`.plist` files** under `/System/Library/LaunchDaemons/` |
| `syslogd(8)` is the FreeBSD-base one | **Apple's `syslogd`** (Apple System Logger / ASL) plus `notifyd` for the cross-process event bus |
| Hardware events surfaced via `devd(8)` (when present) | **In-kernel IORegistry** (`/dev/ioregistry`) with an IOKit-shaped notification channel, browsed via Apple's `libIOKit` / `ioreg` |
| `dhclient(8)` brings up network interfaces | **`ipconfigd`** (Apple's IPConfiguration) handles DHCPv4 + ARP probing + lease renewal + publishes to `SCDynamicStore` |
| `mdnsd` (if installed) for Bonjour | **Apple's `mDNSResponder`** with its full client API |
| Nothing equivalent | **`configd`** + `SCDynamicStore` &mdash; the system-wide key/value store every Apple-source daemon expects |
| Nothing equivalent | **Mach IPC** in-kernel via `mach.ko`, plus `libsystem_kernel` / `libdispatch` / `libxpc` / `liblaunch` / `libCoreFoundation` in userland |

## Try it in 5 minutes

1. Grab the latest [continuous release][release]. Download
   `FreeBSD-15.0-RELEASE-amd64-mach-<DATE>.img.zip`.
2. Unzip it (any platform's native tools work &mdash; macOS Finder,
   Windows Explorer, `unzip` on Linux/BSD):

   ```sh
   unzip FreeBSD-15.0-RELEASE-amd64-mach-*.img.zip
   ```

3. Boot it in [QEMU](https://www.qemu.org/) (easiest), or `dd` to a
   USB stick and boot on bare metal:

   ```sh
   # QEMU, UEFI boot, user-mode network (gives the guest 10.0.2.0/24)
   qemu-system-x86_64 \
     -accel kvm -cpu host -m 2048 \
     -bios /usr/share/OVMF/OVMF_CODE.fd \
     -drive file=disk.img,format=raw,if=virtio \
     -nic user,model=e1000 \
     -nographic
   ```

4. Wait ~10 seconds. You'll see launchd start its daemons, DHCP fire
   on `em0`, syslog come up, and the `login:` prompt land. Log in
   as `root` (no password, hit Enter).

[release]: https://github.com/pkgdemon/freebsd-launchd-mach/releases/tag/continuous

## Networking just works

When the image boots, `ipconfigd` (Apple's DHCPv4 client) DHCPs
the first Ethernet interface it finds. In QEMU's default user-mode
network you'll get `10.0.2.15/24`, gateway `10.0.2.2`, DNS via
`10.0.2.3`. On real hardware you'll get whatever your DHCP server
hands out.

Check it the Apple way:

```sh
ipconfig getifaddr em0          # prints the IP address
ipconfig ifcount                # prints the count of interfaces
```

(`ipconfig(8)` here is Apple's tool with the same name and CLI
shape, not FreeBSD's `ifconfig(8)`. Both exist on the image.)

If you want to see the routing table, default route, and so on,
standard FreeBSD `netstat -rn` works as you'd expect.

## Try some commands

These are the visible-to-users pieces of the Apple-source stack
that's running. All work today on this image:

```sh
# launchctl — the Apple service-control tool, manages launchd jobs.
launchctl list                       # show every job launchd knows about
launchctl list com.apple.syslogd     # plist for one specific job

# ipconfig — Apple's IP-config CLI (NOT the same as ifconfig)
ipconfig getifaddr em0
ipconfig ifcount

# ioreg — Apple's hardware-registry browser, over the in-kernel IORegistry
ioreg -l                             # the full registry tree
ioreg -c PCIDevice                   # filter by class
ioreg -n hostb0                      # find by name

# syslog — Apple's log-query tool, served by syslogd's ASL store
syslog -F bsd                        # tail the system log in BSD format
syslog -k Sender ipconfigd           # filter by sender

# Standard FreeBSD tools are all still here
top
ps aux
pkg info
```

## What's running under the hood

```
+----------------+      +----------------+      +------------------+
|   launchctl    |----->|    launchd     |<-----| .plist files in  |
| /bin/launchctl |      |  /sbin/launchd |      | /System/Library/ |
+----------------+      | (PID 1)        |      |   LaunchDaemons/ |
                        +----------------+      +------------------+
                                |
                                | starts + monitors
                                v
   +----------------+----------------+---------------+---------------+
   | syslogd + ASL  |   notifyd      | mDNSResponder |   configd     |
   | /var/log/...   | event pub/sub  | Bonjour/DNS-SD| SCDynamicStore|
   +----------------+----------------+---------------+---------------+
                                                     |
                                                     | publishes to
                                                     v
                                            +------------------+
                                            |  ipconfigd       |
                                            |  (DHCP + lease)  |
                                            +------------------+
                                                     |
                                                     | + ARP, RA, MIG RPC
                                                     v
                                            +------------------+
                                            |  mDNSResponder   |
                                            |  (Bonjour)       |
                                            +------------------+
```

All of these talk to each other over **Mach IPC** (in-kernel via
`mach.ko`, in userland via `libsystem_kernel.so`). All the daemons
that need a property-list store talk to `configd`'s `SCDynamicStore`
via `libSystemConfiguration.so`.

## Build it yourself

CI builds in a FreeBSD VM via `vmactions/freebsd-vm@v1`, runs the
boot test, and publishes a [continuous release][release] on every
green merge to `main`.

To build locally on FreeBSD:

```sh
sh build.sh
```

You'll get `out/disk.img.zip` &mdash; a DEFLATE-9 zip wrapping a
bootable GPT disk image (BIOS + UEFI, read-write UFS root). Unzip
with any platform's native tools.

## Releases

Every push to `main` that passes build + boot test is published as a
dated continuous release &mdash; the zip-wrapped GPT disk image.
Older builds are kept for two weeks; the latest is always on the
[continuous release page][release].

## Going deeper

- **[PORTING.md](PORTING.md)** &mdash; full technical history of
  what's been ported and why. Phase-by-phase, with per-component
  rationale.
- **[Plan](https://pkgdemon.github.io/freebsd-launchd-mach-plan.html)**
  &mdash; the design doc this project was built from.
- **[Issues](https://github.com/pkgdemon/freebsd-launchd-mach/issues)**
  &mdash; open work items, scoping questions, planned ports.

## License

[BSD-2-Clause](LICENSE), with per-component Apache 2.0 / LGPL / MIT /
OSF / CMU headers preserved on imported files &mdash; see
[NOTICE](NOTICE).

## Companion

The [`freebsd-launchd`](https://github.com/pkgdemon/freebsd-launchd)
repo is the AF_UNIX-only track; this repo is the pure Mach-IPC port
track.
