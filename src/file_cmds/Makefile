# src/file_cmds/Makefile — Apple file_cmds port for freebsd-launchd-mach.
#
# Vendored from apple-oss-distributions/file_cmds@file_cmds-479.
# Builds Apple binaries from per-tool subdirs (chflags/, mkdir/, etc.)
# and installs them at Apple-canonical paths overlay-overwriting
# FreeBSD-runtime's same paths (OpenPAM iter-3 overlay pattern).
#
# Plan: https://pkgdemon.github.io/freebsd-srclist-build-plan.html
#       https://pkgdemon.github.io/freebsd-apple-userland-cmds-plan.html#file_cmds
# Ticket: #111 (#105b file_cmds port)
#
# Iter 1 (merged commit 525541a): 5 leaf tools (chflags, mkdir,
# mkfifo, rmdir, pathchk) — proves the overlay pattern with no shim
# work.
#
# Iter 2 (this): expand to 11 more pure-POSIX tools (cksum, compress,
# dd, ipcrm, ipcs, ln, mknod, rm, shar, touch, truncate). All
# multi-source rules below spell out their .c file lists. shar is
# a shell script that just gets copied.
#
# Iter 3+ (future): cp/mv/ls/chmod/install/gzip/pax/mtree — the
# tools with Apple-specific deps (clonefile, libacl, libdispatch,
# fcopyfile, xattr). Each needs its own shim work.
#
# Usage: make -C src/file_cmds install DESTDIR=$WORK/rootfs

DESTDIR ?=
CFLAGS = -O2 -pipe

ITER1_BINS = chflags/chflags mkdir/mkdir mkfifo/mkfifo rmdir/rmdir \
             pathchk/pathchk
# ipcrm/ipcs DEFERRED — Apple's source uses XNU-specific kernel
# struct layouts (struct msqid_kernel, struct shmid_kernel, union
# semun shape) that FreeBSD's SysV IPC doesn't expose identically.
# Would need a SysV-IPC shim layer. Not load-bearing (we use Mach
# IPC, not SysV) — defer to iter 3+ alongside the libacl/fcopyfile
# shim work.
ITER2_BINS = cksum/cksum compress/compress dd/dd \
             ln/ln mknod/mknod rm/rm \
             touch/touch truncate/truncate

all: $(ITER1_BINS) $(ITER2_BINS)

# --- iter 1 (single-source leaf tools) ---
chflags/chflags: chflags/chflags.c
	cc $(CFLAGS) -o $@ chflags/chflags.c
mkdir/mkdir: mkdir/mkdir.c
	cc $(CFLAGS) -o $@ mkdir/mkdir.c
mkfifo/mkfifo: mkfifo/mkfifo.c
	cc $(CFLAGS) -o $@ mkfifo/mkfifo.c
rmdir/rmdir: rmdir/rmdir.c
	cc $(CFLAGS) -o $@ rmdir/rmdir.c
pathchk/pathchk: pathchk/pathchk.c
	cc $(CFLAGS) -o $@ pathchk/pathchk.c

# --- iter 2 (more pure-POSIX tools; multi-source as needed) ---

# cksum: pure POSIX. Sources: cksum.c, crc.c, crc32.c, print.c,
# sum1.c, sum2.c.
cksum/cksum: cksum/cksum.c cksum/crc.c cksum/crc32.c cksum/print.c \
             cksum/sum1.c cksum/sum2.c
	cc $(CFLAGS) -o $@ cksum/cksum.c cksum/crc.c cksum/crc32.c \
	    cksum/print.c cksum/sum1.c cksum/sum2.c

# compress: BSD compress(1). Sources: compress.c, zopen.c.
compress/compress: compress/compress.c compress/zopen.c
	cc $(CFLAGS) -o $@ compress/compress.c compress/zopen.c

# dd: POSIX. Sources: args.c, conv.c, conv_tab.c, dd.c, misc.c,
# position.c. (gen.c is a standalone test-data generator — has its
# own main() and isn't linked into dd; skip it.) Links libutil for
# humanize_number().
dd/dd: dd/args.c dd/conv.c dd/conv_tab.c dd/dd.c \
       dd/misc.c dd/position.c
	cc $(CFLAGS) -o $@ dd/args.c dd/conv.c dd/conv_tab.c dd/dd.c \
	    dd/misc.c dd/position.c -lutil

# ipcrm/ipcs DEFERRED — see ITER2_BINS comment above.

# ln: POSIX symlink/hardlink.
ln/ln: ln/ln.c
	cc $(CFLAGS) -o $@ ln/ln.c

# mknod: POSIX. Sources: mknod.c, pack_dev.c. Apple uses __dead (NetBSD-
# ism) for non-returning functions; FreeBSD has __dead2. -D__dead=__dead2
# is the one-line bridge.
mknod/mknod: mknod/mknod.c mknod/pack_dev.c
	cc $(CFLAGS) -D__dead=__dead2 -o $@ mknod/mknod.c mknod/pack_dev.c

# rm: POSIX (with FreeBSD-style -P secure-overwrite extension).
rm/rm: rm/rm.c
	cc $(CFLAGS) -o $@ rm/rm.c

# touch: POSIX.
touch/touch: touch/touch.c
	cc $(CFLAGS) -o $@ touch/touch.c

# truncate: POSIX. Links libutil for expand_number / expand_unsigned
# (FreeBSD's <libutil.h> wraps expand_number to expand_unsigned via
# an inline; needs the lib at link time).
truncate/truncate: truncate/truncate.c
	cc $(CFLAGS) -o $@ truncate/truncate.c -lutil

# shar is a shell script (no compile); install handled below.

install: all
	mkdir -p $(DESTDIR)/bin $(DESTDIR)/usr/bin $(DESTDIR)/sbin
	# iter 1
	install -m 0555 chflags/chflags $(DESTDIR)/bin/chflags
	install -m 0555 mkdir/mkdir $(DESTDIR)/bin/mkdir
	install -m 0555 mkfifo/mkfifo $(DESTDIR)/bin/mkfifo
	install -m 0555 rmdir/rmdir $(DESTDIR)/bin/rmdir
	install -m 0555 pathchk/pathchk $(DESTDIR)/usr/bin/pathchk
	# iter 2: file ops + POSIX utils
	install -m 0555 dd/dd $(DESTDIR)/bin/dd
	install -m 0555 ln/ln $(DESTDIR)/bin/ln
	install -m 0555 rm/rm $(DESTDIR)/bin/rm
	install -m 0555 cksum/cksum $(DESTDIR)/usr/bin/cksum
	install -m 0555 compress/compress $(DESTDIR)/usr/bin/compress
	# ipcrm/ipcs DEFERRED — see ITER2_BINS comment above.
	install -m 0555 mknod/mknod $(DESTDIR)/sbin/mknod
	install -m 0555 touch/touch $(DESTDIR)/usr/bin/touch
	install -m 0555 truncate/truncate $(DESTDIR)/usr/bin/truncate
	# shar: shell script
	install -m 0555 shar/shar.sh $(DESTDIR)/usr/bin/shar

clean:
	rm -f $(ITER1_BINS) $(ITER2_BINS)

.PHONY: all clean install
