#!/rescue/sh
# /init.sh — boot pivot script.
#
# Phase A target: hand PID 1 to STOCK FreeBSD /sbin/init after the unionfs
# pivot. No launchd yet; that comes post-checkpoint (Phase D). Two modes:
#
# (a) PID 1 mode (kernel exec'd us via shebang):
#     loader.conf init_path=/init.sh causes the kernel to exec us
#     directly via the shebang above. /rescue/sh becomes PID 1 with
#     this script as argv[1]. After the unionfs pivot we hand PID 1
#     to /sbin/init via `exec /rescue/chroot /sysroot /sbin/init`,
#     preserving PID across the exec chain.
#
# (b) Fallback mode (kernel rejected /init.sh, tried /rescue/init next):
#     /rescue/init runs as PID 1 and reads init_script=/init.sh kenv
#     to invoke us as its child. After the unionfs pivot we set
#     init_chroot=/sysroot kenv and exit; /rescue/init does the chroot
#     and continues normal multi-user boot.
#
# Detection: $$ == 1 iff PID 1 mode.
#
# Memory cost: ~50 MB at idle (decompressed pages of accessed uzip data
# only). Writable upper is tmpfs, page-allocated on demand, no fixed
# size — apparent free space scales with host RAM.

# When running as PID 1 (kernel exec'd us via shebang), kernel hands us
# fds 0/1/2 closed and /dev/console isn't accessible until devfs is
# mounted. Set both up before anything else: stdout silenced (clean
# boot console), stderr to /dev/console so any failure is still visible.
# Use absolute paths because PATH is not yet set.
/rescue/mount -t devfs devfs /dev 2>/dev/null || true
if [ -c /dev/console ]; then
    exec </dev/null >/dev/null 2>/dev/console
fi

set -eu
PATH=/rescue
export PATH

# Defensive module loads (also requested in /boot/loader.conf, but be safe
# in case someone built a kernel without the loader.conf entries).
kldload geom_uzip 2>/dev/null || true
kldload unionfs 2>/dev/null || true

# Vnode-mount the compressed rootfs from the cd9660. /rootfs.uzip is at
# the root of the cd9660 (placed there by build.sh). geom_uzip auto-tastes
# /dev/md0 and produces /dev/md0.uzip.
mdconfig -a -t vnode -o readonly -f /rootfs.uzip -u 0

# Wait for the uzip taste to complete
i=0
while [ ! -e /dev/md0.uzip ]; do
    sleep 1
    i=$((i+1))
    if [ "$i" -gt 30 ]; then
        ls -la /dev/md* 2>/dev/null || true
        halt -p
    fi
done

# Mount the read-only lower at /sysroot (the merge target). /sysroot
# exists as an empty directory on the cd9660 — we can't mkdir it here
# because cd9660 is read-only at runtime.
mount -t ufs -o ro /dev/md0.uzip /sysroot

# Mount tmpfs as the writable upper. tmpfs has no fixed size — pages
# allocate on demand from the VM subsystem and spill to swap under
# memory pressure (semantically identical to Linux tmpfs). The /upper
# mountpoint is also pre-created on the cd9660 by build.sh.
mount -t tmpfs tmpfs /upper

# Layer the tmpfs upper on top of the read-only lower via in-kernel
# unionfs. After this mount, /sysroot is the merged writable view:
# reads fall through to the lower, writes are captured in the upper.
mount -t unionfs /upper /sysroot

# devfs in the chroot
mount -t devfs devfs /sysroot/dev

# Branch on whether we're PID 1 or init's child.
if [ "$$" = "1" ]; then
    # We're PID 1. Hand off to stock FreeBSD /sbin/init by chroot+exec.
    # exec replaces this shell with /rescue/chroot (preserving PID 1),
    # which chroots and execs /sbin/init (still PID 1). init inherits.
    # Standard FreeBSD multi-user boot proceeds from there.
    #
    # Forward "$@" to /sbin/init so loader-time flags survive the pivot.
    # When the user types `boot -s` at the loader prompt, the kernel
    # appends "-s" to init's argv. Via shebang resolution, that lands
    # as $1 of this script. Without forwarding, the chroot'd init sees
    # empty argv, ignores RB_SINGLE, and boots multi-user despite the
    # request. Same forwarding handles -d (init dialog), -r (read-only
    # root retry), -f (fast boot — skip fsck), etc.
    #
    # Unset init_script / init_shell kenvs first: stock /sbin/init reads
    # init_script at startup and forks a child to run it. If the kenv is
    # still set to "/init.sh" (from loader.conf) when we exec init in the
    # chroot, init's child looks for /init.sh INSIDE the chroot (where it
    # doesn't exist) and bombs out to single-user mode. Same fix as the
    # fallback path below uses.
    #
    # Phase D (2026-05-16): swap /sbin/init for /sbin/launchd as PID 1.
    # launchd takes over the full init role -- mounts, getty spawning
    # via LaunchDaemons plists (org.freebsd.getty), service supervision.
    # Stock FreeBSD rc.d is bypassed; everything goes through plists at
    # /System/Library/LaunchDaemons/. Iterate boot-test failures until
    # launchd + getty plist combo brings up login.
    kenv -u init_script 2>/dev/null || true
    kenv -u init_shell  2>/dev/null || true

    # Defensive: a crashing daemon whose fdtable contains mach.ko
    # entries can trip a FreeBSD kernel page fault in
    # kern_proc_filedesc_out during coredump (Phase J runtime iter 1
    # showed this on a syslogd SEGV). Disable coredumps system-wide
    # until mach.ko's fd ops are coredump-safe.
    sysctl kern.coredump=0 2>/dev/null || true

    exec /rescue/chroot /sysroot /sbin/launchd "$@"
fi

# Fallback path: we're a child of /rescue/init via init_script. Set
# kenv for /rescue/init's chroot, then exit. init.c reads init_chroot
# at line 333, AFTER the script runs (lines 326-331), so a kenv set
# here is honored.
kenv init_chroot=/sysroot

# Unset init_script so init doesn't try to re-run us after the chroot.
# (init only reads init_script once at startup, but unset for cleanliness.)
kenv -u init_script 2>/dev/null || true
kenv -u init_shell  2>/dev/null || true

exit 0
