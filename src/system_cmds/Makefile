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

# Iter 2 scope: 4 leaf-ish tools using only POSIX pwd/grp + Apple's
# vendored pw_scan/pw_util (matches FreeBSD's libutil semantics):
#   newgrp  — change effective group ID.
#   vifs    — edit /etc/fstab under a lock (single-source).
#   vipw    — edit /etc/master.passwd under a lock (2 .c).
#   accton  — toggle system accounting via acct(2).
# pwd_mkdb DEFERRED — Apple's wire format must exactly match what
# FreeBSD libc reads from /etc/pwd.db (it does in practice, but
# overlaying a load-bearing auth tool needs its own iter for safety).
# ac DEFERRED — uses non-standard getutxent_wtmp() (same as last).
#
# Iter 3+: most of the remaining ~42 binaries fall into one of these
# buckets — defer until the right shim/trap is in place:
#   - Mach-IPC introspection (lsmp, hostinfo, stackshot, ltop, zprint,
#     vm_stat, vm_purgeable_stat, memory_pressure, purge) — need
#     new mach.ko traps; backfill last per plan §15.
#   - kdebug-based (fs_usage, sc_usage, latency, iosim, kpgo) — defer.
#   - proc_info-based (gcore, taskpolicy, proc_uuid_policy) — defer.
#   - IOPMLib-backed (reboot, shutdown, halt, dynamic_pager) — defer
#     until PowerManagement repo lands the IOPMLib shim.
#   - IOKit-NVRAM (nvram) — defer until IOKit NVRAM bridge.
#   - BSM-audit (login, sa) — defer until BSM port.
#   - Mach-O multi-arch (arch) — N/A on ELF FreeBSD; defer.
#   - chpass/passwd — Open Directory backend; defer with libopendirectory.
#   - getconf — gperf-driven table build; defer to its own iter.

ITER1_BINS = mkfile/mkfile sync/sync wait4path/wait4path
ITER2_BINS = newgrp/newgrp vifs/vifs vipw/vipw accton/accton

all: $(ITER1_BINS) $(ITER2_BINS)

# --- iter 1 (3 C tools; pagesize is a shell script, install-only) ---
mkfile/mkfile: mkfile/mkfile.c
	cc $(CFLAGS) -o $@ mkfile/mkfile.c
sync/sync: sync/sync.c
	cc $(CFLAGS) -o $@ sync/sync.c
wait4path/wait4path: wait4path/wait4path.c
	cc $(CFLAGS) -o $@ wait4path/wait4path.c

# --- iter 2 ---
newgrp/newgrp: newgrp/newgrp.c
	cc $(CFLAGS) -o $@ newgrp/newgrp.c -lutil -lcrypt
vifs/vifs: vifs/vifs.c
	cc $(CFLAGS) -o $@ vifs/vifs.c
# vipw: vipw.c + pw_util.c (Apple's vendored pw_util).
vipw/vipw: vipw/vipw.c vipw/pw_util.c
	cc $(CFLAGS) -o $@ vipw/vipw.c vipw/pw_util.c
accton/accton: accton/accton.c
	cc $(CFLAGS) -o $@ accton/accton.c

install: all
	mkdir -p $(DESTDIR)/bin $(DESTDIR)/usr/bin $(DESTDIR)/usr/sbin
	install -m 0555 mkfile/mkfile $(DESTDIR)/usr/sbin/mkfile
	install -m 0555 sync/sync $(DESTDIR)/bin/sync
	install -m 0555 wait4path/wait4path $(DESTDIR)/bin/wait4path
	install -m 0555 pagesize/pagesize.sh $(DESTDIR)/usr/bin/pagesize
	# iter 2
	install -m 0555 newgrp/newgrp $(DESTDIR)/usr/bin/newgrp
	install -m 0555 vifs/vifs $(DESTDIR)/usr/sbin/vifs
	install -m 0555 vipw/vipw $(DESTDIR)/usr/sbin/vipw
	install -m 0555 accton/accton $(DESTDIR)/usr/sbin/accton

clean:
	rm -f $(ITER1_BINS) $(ITER2_BINS)

.PHONY: all clean install
