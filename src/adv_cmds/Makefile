# src/adv_cmds/Makefile — Apple adv_cmds port for freebsd-launchd-mach.
#
# Vendored from apple-oss-distributions/adv_cmds@adv_cmds-237.
# Builds Apple binaries from per-tool subdirs and installs them at
# Apple-canonical paths overlay-overwriting FreeBSD-runtime/utilities'
# same paths (OpenPAM iter-3 overlay pattern).
#
# Plan: https://pkgdemon.github.io/freebsd-apple-userland-cmds-plan.html#adv_cmds
# Ticket: #113 (#105d adv_cmds port)
#
# Iter 1 scope: 5 leaf tools with no Apple-specific kernel-struct deps
# — tabs (terminal tabstop), tty (ttyname), whois (RFC-3912 socket
# client), gencat (POSIX message catalogs, multi-source), lsvfs
# (getvfsbyname listing).
#
# Iter 2+: cap_mkdb, finger, last, locale, localedef, genwrap, plus
# the load-bearing tools (ps + pkill — need Apple kinfo_proc shim,
# stty — POSIX but minor Apple flag additions).
#
# Usage: make -C src/adv_cmds install DESTDIR=$WORK/rootfs

DESTDIR ?=
CFLAGS = -O2 -pipe -D__dead=__dead2

ITER1_BINS = tabs/tabs tty/tty whois/whois gencat/gencat lsvfs/lsvfs

all: $(ITER1_BINS)

# --- iter 1 (5 single-source-or-near-leaf tools) ---
tabs/tabs: tabs/tabs.c
	cc $(CFLAGS) -o $@ tabs/tabs.c
tty/tty: tty/tty.c
	cc $(CFLAGS) -o $@ tty/tty.c
whois/whois: whois/whois.c
	cc $(CFLAGS) -o $@ whois/whois.c
# gencat: gencat.c + genlib.c (multi-source).
gencat/gencat: gencat/gencat.c gencat/genlib.c
	cc $(CFLAGS) -o $@ gencat/gencat.c gencat/genlib.c
lsvfs/lsvfs: lsvfs/lsvfs.c
	cc $(CFLAGS) -o $@ lsvfs/lsvfs.c

install: all
	mkdir -p $(DESTDIR)/usr/bin $(DESTDIR)/usr/sbin
	install -m 0555 tabs/tabs $(DESTDIR)/usr/bin/tabs
	install -m 0555 tty/tty $(DESTDIR)/usr/bin/tty
	install -m 0555 whois/whois $(DESTDIR)/usr/bin/whois
	install -m 0555 gencat/gencat $(DESTDIR)/usr/bin/gencat
	install -m 0555 lsvfs/lsvfs $(DESTDIR)/usr/sbin/lsvfs

clean:
	rm -f $(ITER1_BINS)

.PHONY: all clean install
