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
# Iter 1 scope: 5 leaf tools (chflags, mkdir, mkfifo, rmdir, pathchk)
# — all confirmed pure POSIX with no Apple-specific syscalls beyond a
# trivial #ifdef __APPLE__ chflagsat shim. Validates the overlay-
# overwrite pattern works for Apple binaries before tackling tools
# with shim work (cp clonefile, ls -e ACL, install xattr, gzip
# fcopyfile, mtree libdispatch, etc.).
#
# Usage: make -C src/file_cmds install DESTDIR=$WORK/rootfs

DESTDIR ?=
CFLAGS = -O2 -pipe

all: chflags/chflags mkdir/mkdir mkfifo/mkfifo rmdir/rmdir pathchk/pathchk

chflags/chflags: chflags/chflags.c
	cc $(CFLAGS) -o $@ $<
mkdir/mkdir: mkdir/mkdir.c
	cc $(CFLAGS) -o $@ $<
mkfifo/mkfifo: mkfifo/mkfifo.c
	cc $(CFLAGS) -o $@ $<
rmdir/rmdir: rmdir/rmdir.c
	cc $(CFLAGS) -o $@ $<
pathchk/pathchk: pathchk/pathchk.c
	cc $(CFLAGS) -o $@ $<

install: all
	mkdir -p $(DESTDIR)/bin $(DESTDIR)/usr/bin
	install -m 0555 chflags/chflags $(DESTDIR)/bin/chflags
	install -m 0555 mkdir/mkdir $(DESTDIR)/bin/mkdir
	install -m 0555 mkfifo/mkfifo $(DESTDIR)/bin/mkfifo
	install -m 0555 rmdir/rmdir $(DESTDIR)/bin/rmdir
	install -m 0555 pathchk/pathchk $(DESTDIR)/usr/bin/pathchk

clean:
	rm -f chflags/chflags mkdir/mkdir mkfifo/mkfifo rmdir/rmdir pathchk/pathchk

.PHONY: all clean install
