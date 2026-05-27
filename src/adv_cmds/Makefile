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
# Iter 2 scope: cap_mkdb (terminfo db builder), finger (network finger
# client, multi-source POSIX). last DEFERRED — Apple uses non-standard
# getutxent_wtmp() which isn't in FreeBSD's libc.
#
# Iter 3+: locale (locale.cc — C++), localedef (10 .c), genwrap (yacc/lex),
# plus the load-bearing tools (ps + pkill — need Apple kinfo_proc shim,
# stty — POSIX but minor Apple flag additions).
#
# Usage: make -C src/adv_cmds install DESTDIR=$WORK/rootfs

DESTDIR ?=
CFLAGS = -O2 -pipe -D__dead=__dead2

# gencat DEFERRED — needs Apple-internal <msgcat.h> header for NLS
# catalog generation (not present on FreeBSD). Would need a shim
# matching Apple's libc/nlsmsg internals. Not load-bearing — defer.
ITER1_BINS = tabs/tabs tty/tty whois/whois lsvfs/lsvfs
ITER2_BINS = cap_mkdb/cap_mkdb finger/finger

all: $(ITER1_BINS) $(ITER2_BINS)

# --- iter 1 (4 single-source-or-near-leaf tools) ---
tabs/tabs: tabs/tabs.c
	cc $(CFLAGS) -o $@ tabs/tabs.c -lncurses
tty/tty: tty/tty.c
	cc $(CFLAGS) -o $@ tty/tty.c
whois/whois: whois/whois.c
	cc $(CFLAGS) -o $@ whois/whois.c
# gencat DEFERRED — see ITER1_BINS comment above.
lsvfs/lsvfs: lsvfs/lsvfs.c
	cc $(CFLAGS) -o $@ lsvfs/lsvfs.c

# --- iter 2 ---
cap_mkdb/cap_mkdb: cap_mkdb/cap_mkdb.c
	cc $(CFLAGS) -o $@ cap_mkdb/cap_mkdb.c
# finger: 5 .c (finger, lprint, net, sprint, util).
finger/finger: finger/finger.c finger/lprint.c finger/net.c finger/sprint.c finger/util.c
	cc $(CFLAGS) -o $@ finger/finger.c finger/lprint.c finger/net.c \
	    finger/sprint.c finger/util.c

install: all
	mkdir -p $(DESTDIR)/usr/bin $(DESTDIR)/usr/sbin
	install -m 0555 tabs/tabs $(DESTDIR)/usr/bin/tabs
	install -m 0555 tty/tty $(DESTDIR)/usr/bin/tty
	install -m 0555 whois/whois $(DESTDIR)/usr/bin/whois
	# gencat DEFERRED — Apple-internal msgcat.h header missing.
	install -m 0555 lsvfs/lsvfs $(DESTDIR)/usr/sbin/lsvfs
	# iter 2
	install -m 0555 cap_mkdb/cap_mkdb $(DESTDIR)/usr/bin/cap_mkdb
	install -m 0555 finger/finger $(DESTDIR)/usr/bin/finger

clean:
	rm -f $(ITER1_BINS) $(ITER2_BINS)

.PHONY: all clean install
