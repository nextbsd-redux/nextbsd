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
# Iter 3 scope (this):
#   getconf — POSIX configuration query. 6 .gperf tables fed through
#     the vendored fake-gperf.awk (state machine emitting a static
#     wordlist[] and an in_word_set() lookup per table) → 6 generated
#     .c files compiled alongside getconf.c. No gperf(1) needed.
# dmesg DEFERRED — Apple's dmesg uses proc_kmsgbuf() (libproc) instead
#   of the kern.msg_buf sysctl path; needs a libproc shim.
# sysctl DEFERRED — Apple's source is FreeBSD's sysctl with __APPLE__
#   add-ons; on FreeBSD it compiles back to the FreeBSD shape, so the
#   overlay adds nothing. Flip in only if Apple-divergent flags useful.
# pwd_mkdb DEFERRED — load-bearing auth-DB writer; own iter for safety.
# ac DEFERRED — uses non-standard getutxent_wtmp() (same as last).
#
# Iter 4 (this): getty. main.c + subr.c + chat.c + init.c. All FreeBSD-
#   side paths exist in the upstream source — Apple-only sites
#   (<util.h>, <TargetConditionals.h>, O_POPUP on /dev/console,
#   "-fp1" login flag, cfmakesane vs TTYDEF_*) are __APPLE__-guarded.
#   Replaces FreeBSD-runtime's /usr/libexec/getty (overlay-overwrite).
#   Used by com.apple.getty.plist on /dev/console. Subsumes the getty
#   half of the (closed) #106 ticket.
#
# Iter 5 (this): three FreeBSD-runtime binaries replaced with Apple's
#   to keep moving FreeBSD-runtime content under our build:
#   - pwd_mkdb — auth-DB writer (single-source pair: pwd_mkdb.c +
#     pw_scan.c). Apple's wire format matches FreeBSD libc's read
#     format per upstream notes.
#   - login — /usr/bin/login. Links libbsm (now from fbsdglue) +
#     libpam (Apple OpenPAM overlay) + libutil. login_audit.c is
#     __APPLE__-guarded for audit_token calls; the !__APPLE__ paths
#     are present in upstream.
#   - passwd — /usr/bin/passwd. Builds with file_passwd.c +
#     nis_passwd.c + pam_passwd.c + a local stub od_passwd_stub.c
#     (we don't have libopendirectory; the stub returns failure so
#     the OD dispatch case in passwd.c degrades to "OpenDirectory
#     backend not built"). PAM is the default infosystem anyway.
#
# Iter 6+: most of the remaining ~37 binaries fall into one of these
# buckets — defer until the right shim/trap is in place:
#   - Mach-IPC introspection (lsmp, hostinfo, stackshot, ltop, zprint,
#     vm_stat, vm_purgeable_stat, memory_pressure, purge) — need
#     new mach.ko traps; backfill last per plan §15.
#   - kdebug-based (fs_usage, sc_usage, latency, iosim, kpgo) — defer.
#   - proc_info-based (gcore, taskpolicy, proc_uuid_policy, dmesg) — defer.
#   - IOPMLib-backed (reboot, shutdown, halt, dynamic_pager) — defer
#     until PowerManagement repo lands the IOPMLib shim.
#   - IOKit-NVRAM (nvram) — defer until IOKit NVRAM bridge.
#   - BSM-audit (login, sa) — defer until BSM port.
#   - Mach-O multi-arch (arch) — N/A on ELF FreeBSD; defer.
#   - chpass/passwd — Open Directory backend; defer with libopendirectory.

ITER1_BINS = mkfile/mkfile sync/sync wait4path/wait4path
ITER2_BINS = newgrp/newgrp vifs/vifs vipw/vipw accton/accton
ITER3_BINS = getconf/getconf
ITER4_BINS = getty/getty
ITER5_BINS = pwd_mkdb/pwd_mkdb login/login passwd/passwd

# Generated by fake-gperf.awk from the .gperf files at build time.
# Each .gperf becomes one .c with a static wordlist[] + in_word_set();
# the trailing find_*/foreach_* helpers (post-second-%%) pass through.
GETCONF_GEN = getconf/confstr.c getconf/limits.c getconf/pathconf.c \
              getconf/progenv.c getconf/sysconf.c getconf/unsigned_limits.c

all: $(ITER1_BINS) $(ITER2_BINS) $(ITER3_BINS) $(ITER4_BINS) $(ITER5_BINS)

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
# vipw: just vipw.c (Apple's vendored pw_util.c conflicts with FreeBSD
# libutil's pw_scan signature — vipw.c uses libutil's pw_init/pw_lock/
# pw_tmp/pw_fini/pw_edit, all in -lutil).
vipw/vipw: vipw/vipw.c
	cc $(CFLAGS) -o $@ vipw/vipw.c -lutil
accton/accton: accton/accton.c
	cc $(CFLAGS) -o $@ accton/accton.c

# --- iter 3 ---
# fake-gperf.awk: state-machine awk pass that transforms a .gperf
# source into a .c that defines a `static const struct map wordlist[]`
# and an `in_word_set(name)` linear-search lookup. The post-second-%%
# body (find_<table>() / foreach_<table>() helpers) is passed through
# verbatim, so each generated .c is self-contained.
#
# bmake doesn't expand $< in explicit rules — spell out paths
# (same fix as file_cmds commit f06182e).
getconf/confstr.c: getconf/confstr.gperf getconf/fake-gperf.awk
	awk -f getconf/fake-gperf.awk < getconf/confstr.gperf > getconf/confstr.c
getconf/limits.c: getconf/limits.gperf getconf/fake-gperf.awk
	awk -f getconf/fake-gperf.awk < getconf/limits.gperf > getconf/limits.c
getconf/pathconf.c: getconf/pathconf.gperf getconf/fake-gperf.awk
	awk -f getconf/fake-gperf.awk < getconf/pathconf.gperf > getconf/pathconf.c
getconf/progenv.c: getconf/progenv.gperf getconf/fake-gperf.awk
	awk -f getconf/fake-gperf.awk < getconf/progenv.gperf > getconf/progenv.c
getconf/sysconf.c: getconf/sysconf.gperf getconf/fake-gperf.awk
	awk -f getconf/fake-gperf.awk < getconf/sysconf.gperf > getconf/sysconf.c
getconf/unsigned_limits.c: getconf/unsigned_limits.gperf getconf/fake-gperf.awk
	awk -f getconf/fake-gperf.awk < getconf/unsigned_limits.gperf > getconf/unsigned_limits.c

getconf/getconf: getconf/getconf.c $(GETCONF_GEN)
	cc $(CFLAGS) -o $@ getconf/getconf.c $(GETCONF_GEN)

# --- iter 4 ---
# getty: 4 .c (main, subr, chat, init). All Apple-only sites
# (<util.h>, <TargetConditionals.h>, O_POPUP on /dev/console,
# "-fp1" login flag, cfmakesane vs TTYDEF_*) are __APPLE__-guarded;
# building without __APPLE__ picks the FreeBSD paths. -lutil for
# login_tty.
getty/getty: getty/main.c getty/subr.c getty/chat.c getty/init.c
	cc $(CFLAGS) -o $@ getty/main.c getty/subr.c getty/chat.c \
	    getty/init.c -lutil

# --- iter 5 ---
# pwd_mkdb: 2 .c (pwd_mkdb.c + pw_scan.c). Apple's wire format
# matches FreeBSD libc's read format — overlays the FreeBSD-runtime
# binary at /usr/sbin/pwd_mkdb.
pwd_mkdb/pwd_mkdb: pwd_mkdb/pwd_mkdb.c pwd_mkdb/pw_scan.c
	cc $(CFLAGS) -o $@ pwd_mkdb/pwd_mkdb.c pwd_mkdb/pw_scan.c

# login: login.c + login_audit.c. Links libpam (Apple OpenPAM overlay)
# + libbsm (fbsdglue) + libutil (login_tty etc.). -DLOGIN_CAP matches
# FreeBSD's standard usr.bin/login build — the upstream login.c uses
# <login_cap.h> + setusercontext() in the !__APPLE__ branch, but those
# are gated on #ifdef LOGIN_CAP in the declarations.
login/login: login/login.c login/login_audit.c
	cc $(CFLAGS) -DLOGIN_CAP -DUSE_PAM -o $@ \
	    login/login.c login/login_audit.c \
	    -lpam -lbsm -lutil

# passwd: passwd.c + file_passwd.c + nis_passwd.c + pam_passwd.c +
# od_passwd_stub.c. Skips the real od_passwd.c (which #include's
# <OpenDirectory/*.h>); the stub returns 1 so `passwd -i OpenDirectory`
# prints an error and exits non-zero. PAM is the default infosystem.
# Links libpam (OpenPAM) + libutil.
passwd/passwd: passwd/passwd.c passwd/file_passwd.c passwd/nis_passwd.c \
               passwd/pam_passwd.c passwd/od_passwd_stub.c
	cc $(CFLAGS) -o $@ passwd/passwd.c passwd/file_passwd.c \
	    passwd/nis_passwd.c passwd/pam_passwd.c \
	    passwd/od_passwd_stub.c -lpam -lutil

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
	# iter 3
	install -m 0555 getconf/getconf $(DESTDIR)/usr/bin/getconf
	# iter 4
	mkdir -p $(DESTDIR)/usr/libexec
	install -m 0555 getty/getty $(DESTDIR)/usr/libexec/getty
	# iter 5
	install -m 0555 pwd_mkdb/pwd_mkdb $(DESTDIR)/usr/sbin/pwd_mkdb
	install -m 4555 login/login $(DESTDIR)/usr/bin/login
	install -m 4555 passwd/passwd $(DESTDIR)/usr/bin/passwd

clean:
	rm -f $(ITER1_BINS) $(ITER2_BINS) $(ITER3_BINS) $(ITER4_BINS) \
	      $(ITER5_BINS) $(GETCONF_GEN)

.PHONY: all clean install
