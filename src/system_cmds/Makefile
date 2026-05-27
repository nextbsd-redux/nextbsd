# src/system_cmds/Makefile — Apple system_cmds port for freebsd-launchd-mach.
#
# Vendored from apple-oss-distributions/system_cmds@system_cmds-1042.100.6.0.1.
# Builds Apple binaries from per-tool subdirs and installs them at
# Apple-canonical paths overlay-overwriting FreeBSD-runtime/utilities'
# same paths (OpenPAM iter-3 overlay pattern).
#
# Plan: https://pkgdemon.github.io/freebsd-apple-userland-cmds-plan.html#system_cmds
# Ticket: #115 (#105g system_cmds port)
#
# Iter 1 scope: 4 trivial leaf tools that don't need Mach IPC traps
# or Apple-specific kernel-struct shims —
#   mkfile (file allocation via ftruncate),
#   sync (sync(2) wrapper, identical to FreeBSD's /bin/sync),
#   wait4path (kqueue EVFILT_VNODE poll for path appearance),
#   pagesize (sh wrapper around sysctl hw.pagesize).
#
# Iter 2+: most of the remaining ~46 binaries fall into one of these
# buckets — defer until the right shim/trap is in place:
#   - Mach-IPC introspection (lsmp, hostinfo, stackshot, ltop, zprint,
#     vm_stat, vm_purgeable_stat, memory_pressure, purge) — need
#     new mach.ko traps; backfill last per plan §15.
#   - kdebug-based (fs_usage, sc_usage, latency, iosim, kpgo) — defer,
#     kdebug not on FreeBSD.
#   - proc_info-based (gcore, taskpolicy, proc_uuid_policy) — defer,
#     needs proc_info shim.
#   - IOPMLib-backed (reboot, shutdown, halt, dynamic_pager) — defer
#     until PowerManagement repo lands the IOPMLib shim.
#   - IOKit-NVRAM (nvram) — defer until IOKit IOService NVRAM bridge.
#   - BSM-audit (login, sa) — defer until BSM port.
#   - Mach-O multi-arch (arch) — N/A on ELF FreeBSD; defer.
#   - sysctl, dmesg — overlap with FreeBSD-utilities equivalents that
#     already work; flip in a later iter only if Apple flags needed.
#   - getconf — gperf-driven table build; useful but mechanically
#     larger; defer to its own iter.
#
# Usage: make -C src/system_cmds install DESTDIR=$WORK/rootfs

DESTDIR ?=
CFLAGS = -O2 -pipe -D__dead=__dead2

ITER1_BINS = mkfile/mkfile sync/sync wait4path/wait4path

all: $(ITER1_BINS)

# --- iter 1 (3 C tools; pagesize is a shell script, install-only) ---
mkfile/mkfile: mkfile/mkfile.c
	cc $(CFLAGS) -o $@ mkfile/mkfile.c
sync/sync: sync/sync.c
	cc $(CFLAGS) -o $@ sync/sync.c
wait4path/wait4path: wait4path/wait4path.c
	cc $(CFLAGS) -o $@ wait4path/wait4path.c

install: all
	mkdir -p $(DESTDIR)/bin $(DESTDIR)/usr/bin $(DESTDIR)/usr/sbin
	install -m 0555 mkfile/mkfile $(DESTDIR)/usr/sbin/mkfile
	install -m 0555 sync/sync $(DESTDIR)/bin/sync
	install -m 0555 wait4path/wait4path $(DESTDIR)/bin/wait4path
	install -m 0555 pagesize/pagesize.sh $(DESTDIR)/usr/bin/pagesize

clean:
	rm -f $(ITER1_BINS)

.PHONY: all clean install
