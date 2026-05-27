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
# Iter 3 (merged): locale (locale.cc — C++).
#
# Iter 4 (this): stty — terminal mode setter. 7 sources, mostly POSIX
# termios; Apple-specific flag/disc references (NTTYDISC, TABLDISC,
# VERASE2, cfmakesane override) are #ifdef'd. Apple's <get_compat.h>
# (modes.c) is shadowed by a local COMPAT_MODE(a,b)=1 fallback.
#
# Iter 5+: localedef (10 .c), genwrap (yacc/lex), ps + pkill (need
# Apple kinfo_proc shim).
#
# Usage: make -C src/adv_cmds install DESTDIR=$WORK/rootfs

DESTDIR ?=
CFLAGS = -O2 -pipe -D__dead=__dead2

# gencat DEFERRED — needs Apple-internal <msgcat.h> header for NLS
# catalog generation (not present on FreeBSD). Would need a shim
# matching Apple's libc/nlsmsg internals. Not load-bearing — defer.
ITER1_BINS = tabs/tabs tty/tty whois/whois lsvfs/lsvfs
ITER2_BINS = cap_mkdb/cap_mkdb finger/finger
# Iter 3: locale (C++, 446 LOC). xlocale.h + langinfo.h are present on
# FreeBSD, and the source uses only C++ STL otherwise.
# localedef DEFERRED — 10 .c with Apple-internal msgcat machinery.
# genwrap DEFERRED — yacc/lex driver.
ITER3_BINS = locale/locale
# Iter 4: stty (7 .c — cchar, gfmt, key, modes, print, stty, util).
ITER4_BINS = stty/stty

all: $(ITER1_BINS) $(ITER2_BINS) $(ITER3_BINS) $(ITER4_BINS)

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

# --- iter 3 ---
# locale: C++ STL, uses xlocale + langinfo. -O2 -pipe with c++.
locale/locale: locale/locale.cc
	c++ -O2 -pipe -o $@ locale/locale.cc

# --- iter 4 ---
# stty: 7 .c. Apple-only blocks (NTTYDISC, TABLDISC, cfmakesane,
# get_compat) are #ifdef __APPLE__-guarded; FreeBSD-only VERASE2
# is added via the !__APPLE__ branch in cchar.c.
stty/stty: stty/cchar.c stty/gfmt.c stty/key.c stty/modes.c \
           stty/print.c stty/stty.c stty/util.c
	cc $(CFLAGS) -o $@ stty/cchar.c stty/gfmt.c stty/key.c \
	    stty/modes.c stty/print.c stty/stty.c stty/util.c

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
	# iter 3
	install -m 0555 locale/locale $(DESTDIR)/usr/bin/locale
	# iter 4
	mkdir -p $(DESTDIR)/bin
	install -m 0555 stty/stty $(DESTDIR)/bin/stty

clean:
	rm -f $(ITER1_BINS) $(ITER2_BINS) $(ITER3_BINS) $(ITER4_BINS)

.PHONY: all clean install
