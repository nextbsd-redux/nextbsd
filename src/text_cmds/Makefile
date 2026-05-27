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
# cat, head, tail, wc, tr.
#
# Iter 2 scope: 17 single-source pure-POSIX stream tools that need
# no extra libs — col, colrm, comm, csplit, cut, expand, fmt, fold,
# nl, paste, rev, ul, unexpand, uniq, lam, look, banner.
#
# Iter 3+: remaining 12 tools (sort, sed, grep, jq, ed, pr, rs,
# split, md5, column, vis, unvis, bintrans, join). sort is the
# headline (uses Mach semaphores). The others need either Apple
# crypto (md5), libregex (grep), libsbuf (column), or non-trivial
# multi-source assembly.
#
# Usage: make -C src/text_cmds install DESTDIR=$WORK/rootfs

DESTDIR ?=
CFLAGS = -O2 -pipe -D__dead=__dead2

ITER1_BINS = cat/cat head/head tail/tail wc/wc tr/tr
ITER2_BINS = col/col colrm/colrm comm/comm csplit/csplit cut/cut \
             expand/expand fmt/fmt fold/fold nl/nl paste/paste \
             rev/rev ul/ul unexpand/unexpand uniq/uniq lam/lam \
             look/look banner/banner
# Iter 3 scope: 12 more tools across single- and multi-source.
#   ed (7 .c) — also produces /usr/bin/red link.
#   pr (2 .c), rs, split (-lutil), vis (2 .c), unvis, join, column,
#   md5 (-lmd; dispatches by argv[0] for sha1/sha224/.../skein* links),
#   bintrans (5 .c, -lresolv; dispatches for base64/uuencode/uudecode/
#   b64encode/b64decode), sed (4 .c), grep (4 .c, -lz -llzma -lbz2).
# sort DEFERRED — file.c uses Mach semaphores for parallel sort,
# needs a libdispatch / Mach IPC binding (the wedge for proving the
# Mach IPC track end-to-end; promote to iter 4 of its own).
# jq is an Apple wrapper dir; nothing to build.
ITER3_BINS = ed/ed pr/pr rs/rs split/split vis/vis unvis/unvis \
             join/join column/column md5/md5 bintrans/bintrans \
             sed/sed grep/grep

all: $(ITER1_BINS) $(ITER2_BINS) $(ITER3_BINS)

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

# --- iter 2 (17 single-source POSIX stream tools, no extra libs) ---
col/col: col/col.c
	cc $(CFLAGS) -o $@ col/col.c
colrm/colrm: colrm/colrm.c
	cc $(CFLAGS) -o $@ colrm/colrm.c
comm/comm: comm/comm.c
	cc $(CFLAGS) -o $@ comm/comm.c
csplit/csplit: csplit/csplit.c
	cc $(CFLAGS) -o $@ csplit/csplit.c
cut/cut: cut/cut.c
	cc $(CFLAGS) -o $@ cut/cut.c
expand/expand: expand/expand.c
	cc $(CFLAGS) -o $@ expand/expand.c
fmt/fmt: fmt/fmt.c
	cc $(CFLAGS) -o $@ fmt/fmt.c
fold/fold: fold/fold.c
	cc $(CFLAGS) -o $@ fold/fold.c
nl/nl: nl/nl.c
	cc $(CFLAGS) -o $@ nl/nl.c
paste/paste: paste/paste.c
	cc $(CFLAGS) -o $@ paste/paste.c
rev/rev: rev/rev.c
	cc $(CFLAGS) -o $@ rev/rev.c
ul/ul: ul/ul.c
	cc $(CFLAGS) -o $@ ul/ul.c -lncurses
unexpand/unexpand: unexpand/unexpand.c
	cc $(CFLAGS) -o $@ unexpand/unexpand.c
uniq/uniq: uniq/uniq.c
	cc $(CFLAGS) -o $@ uniq/uniq.c
lam/lam: lam/lam.c
	cc $(CFLAGS) -o $@ lam/lam.c
look/look: look/look.c
	cc $(CFLAGS) -o $@ look/look.c
banner/banner: banner/banner.c
	cc $(CFLAGS) -o $@ banner/banner.c

# --- iter 3 ---
# ed: 7 .c (buf, glbl, io, main, re, sub, undo).
ed/ed: ed/buf.c ed/glbl.c ed/io.c ed/main.c ed/re.c ed/sub.c ed/undo.c
	cc $(CFLAGS) -o $@ ed/buf.c ed/glbl.c ed/io.c ed/main.c \
	    ed/re.c ed/sub.c ed/undo.c
# pr: pr.c + egetopt.c.
pr/pr: pr/pr.c pr/egetopt.c
	cc $(CFLAGS) -o $@ pr/pr.c pr/egetopt.c
rs/rs: rs/rs.c
	cc $(CFLAGS) -o $@ rs/rs.c
split/split: split/split.c
	cc $(CFLAGS) -o $@ split/split.c -lutil
# vis: vis.c + foldit.c.
vis/vis: vis/vis.c vis/foldit.c
	cc $(CFLAGS) -o $@ vis/vis.c vis/foldit.c
unvis/unvis: unvis/unvis.c
	cc $(CFLAGS) -o $@ unvis/unvis.c
join/join: join/join.c
	cc $(CFLAGS) -o $@ join/join.c
column/column: column/column.c
	cc $(CFLAGS) -o $@ column/column.c
# md5: dispatches by argv[0]. -lmd brings in MD5/SHA1/SHA256/etc.
md5/md5: md5/md5.c
	cc $(CFLAGS) -o $@ md5/md5.c -lmd
# bintrans: 4 .c (bintrans, qp, uudecode, uuencode, apple_base64).
# Dispatches by argv[0] for base64/b64encode/b64decode/uuencode/uudecode.
# -lresolv brings in b64_ntop / b64_pton.
bintrans/bintrans: bintrans/bintrans.c bintrans/qp.c bintrans/uudecode.c \
                   bintrans/uuencode.c bintrans/apple_base64.c
	cc $(CFLAGS) -o $@ bintrans/bintrans.c bintrans/qp.c \
	    bintrans/uudecode.c bintrans/uuencode.c \
	    bintrans/apple_base64.c -lresolv
# sed: 4 .c (compile, main, misc, process).
sed/sed: sed/compile.c sed/main.c sed/misc.c sed/process.c
	cc $(CFLAGS) -o $@ sed/compile.c sed/main.c sed/misc.c \
	    sed/process.c
# grep: 4 .c (file, grep, queue, util). -lz/-llzma/-lbz2 for
# zgrep/xzgrep/bzgrep transparent decompression.
grep/grep: grep/file.c grep/grep.c grep/queue.c grep/util.c
	cc $(CFLAGS) -o $@ grep/file.c grep/grep.c grep/queue.c \
	    grep/util.c -lz -llzma -lbz2

install: all
	mkdir -p $(DESTDIR)/bin $(DESTDIR)/usr/bin $(DESTDIR)/usr/games \
	         $(DESTDIR)/sbin
	# iter 1
	install -m 0555 cat/cat $(DESTDIR)/bin/cat
	install -m 0555 head/head $(DESTDIR)/usr/bin/head
	install -m 0555 tail/tail $(DESTDIR)/usr/bin/tail
	install -m 0555 wc/wc $(DESTDIR)/usr/bin/wc
	install -m 0555 tr/tr $(DESTDIR)/usr/bin/tr
	# iter 2
	install -m 0555 col/col $(DESTDIR)/usr/bin/col
	install -m 0555 colrm/colrm $(DESTDIR)/usr/bin/colrm
	install -m 0555 comm/comm $(DESTDIR)/usr/bin/comm
	install -m 0555 csplit/csplit $(DESTDIR)/usr/bin/csplit
	install -m 0555 cut/cut $(DESTDIR)/usr/bin/cut
	install -m 0555 expand/expand $(DESTDIR)/usr/bin/expand
	install -m 0555 fmt/fmt $(DESTDIR)/usr/bin/fmt
	install -m 0555 fold/fold $(DESTDIR)/usr/bin/fold
	install -m 0555 nl/nl $(DESTDIR)/usr/bin/nl
	install -m 0555 paste/paste $(DESTDIR)/usr/bin/paste
	install -m 0555 rev/rev $(DESTDIR)/usr/bin/rev
	install -m 0555 ul/ul $(DESTDIR)/usr/bin/ul
	install -m 0555 unexpand/unexpand $(DESTDIR)/usr/bin/unexpand
	install -m 0555 uniq/uniq $(DESTDIR)/usr/bin/uniq
	install -m 0555 lam/lam $(DESTDIR)/usr/bin/lam
	install -m 0555 look/look $(DESTDIR)/usr/bin/look
	install -m 0555 banner/banner $(DESTDIR)/usr/games/banner
	# iter 3
	install -m 0555 ed/ed $(DESTDIR)/bin/ed
	ln -f $(DESTDIR)/bin/ed $(DESTDIR)/bin/red
	install -m 0555 pr/pr $(DESTDIR)/usr/bin/pr
	install -m 0555 rs/rs $(DESTDIR)/usr/bin/rs
	install -m 0555 split/split $(DESTDIR)/usr/bin/split
	install -m 0555 vis/vis $(DESTDIR)/usr/bin/vis
	install -m 0555 unvis/unvis $(DESTDIR)/usr/bin/unvis
	install -m 0555 join/join $(DESTDIR)/usr/bin/join
	install -m 0555 column/column $(DESTDIR)/usr/bin/column
	# md5 dispatches by argv[0] — hardlink to sha1/sha224/.../skein.
	install -m 0555 md5/md5 $(DESTDIR)/sbin/md5
	for n in sha1 sha224 sha256 sha384 sha512 sha512t256 rmd160 \
	         skein256 skein512 skein1024; do \
	    ln -f $(DESTDIR)/sbin/md5 $(DESTDIR)/sbin/$$n; \
	done
	# bintrans dispatches by argv[0] for base64/uu*/b64*.
	install -m 0555 bintrans/bintrans $(DESTDIR)/usr/bin/bintrans
	for n in base64 b64encode b64decode uuencode uudecode; do \
	    ln -f $(DESTDIR)/usr/bin/bintrans $(DESTDIR)/usr/bin/$$n; \
	done
	install -m 0555 sed/sed $(DESTDIR)/usr/bin/sed
	# grep — dispatches by argv[0] for egrep/fgrep/zgrep/etc.
	install -m 0555 grep/grep $(DESTDIR)/usr/bin/grep
	for n in egrep fgrep rgrep zgrep zegrep zfgrep \
	         bzgrep bzegrep bzfgrep \
	         xzgrep xzegrep xzfgrep \
	         lzgrep lzegrep lzfgrep; do \
	    ln -f $(DESTDIR)/usr/bin/grep $(DESTDIR)/usr/bin/$$n; \
	done

clean:
	rm -f $(ITER1_BINS) $(ITER2_BINS) $(ITER3_BINS)

.PHONY: all clean install
