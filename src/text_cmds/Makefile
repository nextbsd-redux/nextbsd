# src/text_cmds/Makefile — Apple text_cmds port for freebsd-launchd-mach.
#
# Vendored from apple-oss-distributions/text_cmds@text_cmds-199.
# Builds Apple binaries from per-tool subdirs and installs them at
# Apple-canonical paths overlay-overwriting FreeBSD-runtime/utilities'
# same paths (OpenPAM iter-3 overlay pattern).
#
# Plan: https://pkgdemon.github.io/freebsd-apple-userland-cmds-plan.html#text_cmds
# Ticket: #114 (#105e text_cmds port)
#
# Iter 1 scope: 5 universally-used POSIX stream processors —
# cat, head, tail, wc, tr. Mix of single-source (cat, head, wc)
# and multi-source (tail = 5 .c, tr = 4 .c).
#
# Iter 2+: rest of the 34 tools. sort is the headline (uses Mach
# semaphores for parallel sort — we have Mach already). Most others
# are pure POSIX stream processors (col, colrm, column, comm, csplit,
# cut, ed, expand, fmt, fold, grep, jq, join, lam, look, md5, nl,
# paste, pr, rev, rs, sed, split, ul, unexpand, uniq, unvis, vis,
# banner, bintrans).
#
# Usage: make -C src/text_cmds install DESTDIR=$WORK/rootfs

DESTDIR ?=
CFLAGS = -O2 -pipe -D__dead=__dead2

ITER1_BINS = cat/cat head/head tail/tail wc/wc tr/tr

all: $(ITER1_BINS)

# --- iter 1 (5 POSIX stream tools) ---
cat/cat: cat/cat.c
	cc $(CFLAGS) -o $@ cat/cat.c
head/head: head/head.c
	cc $(CFLAGS) -o $@ head/head.c
# tail: 5 .c files (forward, misc, read, reverse, tail).
tail/tail: tail/forward.c tail/misc.c tail/read.c tail/reverse.c tail/tail.c
	cc $(CFLAGS) -o $@ tail/forward.c tail/misc.c tail/read.c \
	    tail/reverse.c tail/tail.c -lutil
wc/wc: wc/wc.c
	cc $(CFLAGS) -o $@ wc/wc.c -lxo
# tr: 4 .c files (cmap, cset, str, tr).
tr/tr: tr/cmap.c tr/cset.c tr/str.c tr/tr.c
	cc $(CFLAGS) -o $@ tr/cmap.c tr/cset.c tr/str.c tr/tr.c

install: all
	mkdir -p $(DESTDIR)/bin $(DESTDIR)/usr/bin
	install -m 0555 cat/cat $(DESTDIR)/bin/cat
	install -m 0555 head/head $(DESTDIR)/usr/bin/head
	install -m 0555 tail/tail $(DESTDIR)/usr/bin/tail
	install -m 0555 wc/wc $(DESTDIR)/usr/bin/wc
	install -m 0555 tr/tr $(DESTDIR)/usr/bin/tr

clean:
	rm -f $(ITER1_BINS)

.PHONY: all clean install
