# src/shell_cmds/Makefile — Apple shell_cmds port for freebsd-launchd-mach.
#
# Vendored from apple-oss-distributions/shell_cmds@shell_cmds-329.
# Builds Apple binaries from per-tool subdirs and installs them at
# Apple-canonical paths overlay-overwriting FreeBSD-runtime/utilities'
# same paths (OpenPAM iter-3 overlay pattern; same as file_cmds).
#
# Plan: https://pkgdemon.github.io/freebsd-apple-userland-cmds-plan.html#shell_cmds
# Ticket: #112 (#105c shell_cmds port)
#
# Iter 1 (merged commit eeb58dd): 5 trivial leaf tools (true, false,
# echo, sleep, basename) — proves the vendor pattern.
#
# Iter 2 (this): expand to 20 more single-source pure-POSIX tools
# (apply, dirname, env, getopt, hostname, jot, kill, logname, mktemp,
# nice, nohup, printenv, printf, pwd, realpath, renice, tee, uname,
# what, yes). -D__dead=__dead2 added to global CFLAGS preemptively
# (file_cmds iter 2 mknod fix; safer to apply repo-wide).
#
# Iter 3+: load-bearing tools (/bin/sh swap, /usr/bin/id with
# libopendirectory shim, /usr/bin/su with BSM stub, /usr/bin/killall
# with KERN_PROCARGS2). And remaining multi-source tools (date, expr,
# find, hexdump, lastcomm, locate, script, sh, su, w, who, xargs)
# with their own builds.
#
# Usage: make -C src/shell_cmds install DESTDIR=$WORK/rootfs

DESTDIR ?=
CFLAGS = -O2 -pipe -D__dead=__dead2

ITER1_BINS = true/true false/false echo/echo sleep/sleep \
             basename/basename
ITER2_BINS = apply/apply dirname/dirname env/env getopt/getopt \
             hostname/hostname jot/jot kill/kill logname/logname \
             mktemp/mktemp nice/nice nohup/nohup printenv/printenv \
             printf/printf pwd/pwd realpath/realpath renice/renice \
             tee/tee uname/uname what/what yes/yes
ITER3_BINS = chroot/chroot date/date hexdump/hexdump lockf/lockf \
             script/script shlock/shlock stdbuf/stdbuf test/test \
             whereis/whereis which/which xargs/xargs
# Iter 4 scope:
#   find — load-bearing POSIX file-finder (7 .c; getdate.y is orphaned).
#   who  — utmpx-based logged-in users listing.
# w DEFERRED — uses KERN_PROCARGS2 (Apple-specific sysctl) + -lkvm
#   (would need a kvm-style proc shim).
# Remaining deferred buckets — see iter 3 commit message.
ITER4_BINS = find/find who/who
# Iter 5 scope:
#   locate — file-name index search. 3 binaries (locate, locate.bigram,
#     locate.code) + 3 shell scripts (locate.updatedb, locate.concatdb,
#     locate.mklocatedb) + /etc/locate.rc config. locate.c #includes
#     fastfind.c directly (single-TU), so only locate.c + util.c are
#     linked. Apple guards FreeBSD's capsicum_helpers behind !__APPLE__
#     in bigram, picked up automatically on FreeBSD.
#   lastcomm — process-accounting reader. Single-source; links libm
#     for floor/fmod over ac_utime/ac_stime deltas. Pairs with the
#     iter-2 accton(8) toggle.
ITER5_BINS = locate/bigram/locate.bigram locate/code/locate.code \
             locate/locate/locate lastcomm/lastcomm

all: $(ITER1_BINS) $(ITER2_BINS) $(ITER3_BINS) $(ITER4_BINS) $(ITER5_BINS)

# --- iter 1 (single-source pure-POSIX leaf tools) ---
true/true: true/true.c
	cc $(CFLAGS) -o $@ true/true.c
false/false: false/false.c
	cc $(CFLAGS) -o $@ false/false.c
echo/echo: echo/echo.c
	cc $(CFLAGS) -o $@ echo/echo.c
sleep/sleep: sleep/sleep.c
	cc $(CFLAGS) -o $@ sleep/sleep.c
basename/basename: basename/basename.c
	cc $(CFLAGS) -o $@ basename/basename.c

# --- iter 2 (single-source POSIX tools) ---
# Most are pure libc; jot/seq use libm; printf may use libxo (Apple
# extension — skip if causes issues).
apply/apply: apply/apply.c
	cc $(CFLAGS) -o $@ apply/apply.c -lsbuf
dirname/dirname: dirname/dirname.c
	cc $(CFLAGS) -o $@ dirname/dirname.c
# env: env.c + envopts.c (multi-source).
env/env: env/env.c env/envopts.c
	cc $(CFLAGS) -o $@ env/env.c env/envopts.c -lutil
getopt/getopt: getopt/getopt.c
	cc $(CFLAGS) -o $@ getopt/getopt.c
hostname/hostname: hostname/hostname.c
	cc $(CFLAGS) -o $@ hostname/hostname.c
# jot uses libm (math functions).
jot/jot: jot/jot.c
	cc $(CFLAGS) -o $@ jot/jot.c -lm
kill/kill: kill/kill.c
	cc $(CFLAGS) -o $@ kill/kill.c
logname/logname: logname/logname.c
	cc $(CFLAGS) -o $@ logname/logname.c
mktemp/mktemp: mktemp/mktemp.c
	cc $(CFLAGS) -o $@ mktemp/mktemp.c
nice/nice: nice/nice.c
	cc $(CFLAGS) -o $@ nice/nice.c
nohup/nohup: nohup/nohup.c
	cc $(CFLAGS) -o $@ nohup/nohup.c
printenv/printenv: printenv/printenv.c
	cc $(CFLAGS) -o $@ printenv/printenv.c
printf/printf: printf/printf.c
	cc $(CFLAGS) -o $@ printf/printf.c
pwd/pwd: pwd/pwd.c
	cc $(CFLAGS) -o $@ pwd/pwd.c
realpath/realpath: realpath/realpath.c
	cc $(CFLAGS) -o $@ realpath/realpath.c
renice/renice: renice/renice.c
	cc $(CFLAGS) -o $@ renice/renice.c
tee/tee: tee/tee.c
	cc $(CFLAGS) -o $@ tee/tee.c
uname/uname: uname/uname.c
	cc $(CFLAGS) -o $@ uname/uname.c
what/what: what/what.c
	cc $(CFLAGS) -o $@ what/what.c
yes/yes: yes/yes.c
	cc $(CFLAGS) -o $@ yes/yes.c

# --- iter 3 ---
chroot/chroot: chroot/chroot.c
	cc $(CFLAGS) -o $@ chroot/chroot.c
# date: date.c + vary.c
date/date: date/date.c date/vary.c
	cc $(CFLAGS) -o $@ date/date.c date/vary.c
# hexdump: 6 .c (conv, display, hexdump, hexsyntax, odsyntax, parse).
# Also produces /usr/bin/od as a hardlink.
hexdump/hexdump: hexdump/conv.c hexdump/display.c hexdump/hexdump.c \
                 hexdump/hexsyntax.c hexdump/odsyntax.c hexdump/parse.c
	cc $(CFLAGS) -o $@ hexdump/conv.c hexdump/display.c \
	    hexdump/hexdump.c hexdump/hexsyntax.c hexdump/odsyntax.c \
	    hexdump/parse.c
lockf/lockf: lockf/lockf.c
	cc $(CFLAGS) -o $@ lockf/lockf.c
# script: needs libutil (openpty/login_tty).
script/script: script/script.c
	cc $(CFLAGS) -o $@ script/script.c -lutil
shlock/shlock: shlock/shlock.c
	cc $(CFLAGS) -o $@ shlock/shlock.c
stdbuf/stdbuf: stdbuf/stdbuf.c
	cc $(CFLAGS) -o $@ stdbuf/stdbuf.c
# test: single-source; also produces `[` as a hardlink.
test/test: test/test.c
	cc $(CFLAGS) -o $@ test/test.c
whereis/whereis: whereis/whereis.c
	cc $(CFLAGS) -o $@ whereis/whereis.c
which/which: which/which.c
	cc $(CFLAGS) -o $@ which/which.c
# xargs: xargs.c + strnsubst.c
xargs/xargs: xargs/xargs.c xargs/strnsubst.c
	cc $(CFLAGS) -o $@ xargs/xargs.c xargs/strnsubst.c

# --- iter 4 ---
# find: 7 .c (find/function/ls/main/misc/operator/option) + getdate.y
# (yacc-generated date parser used by -newerXY / get_date()).
# Apple guards <unistd.h> behind #ifdef __APPLE__ in find.c (line 53-55)
# but uses X_OK at line 242 — force-include unistd.h to declare it.
# join-style fix: define unix2003_compat=0 so the BSD path is taken.
find/getdate.c: find/getdate.y
	yacc -o $@ find/getdate.y
find/find: find/find.c find/function.c find/ls.c find/main.c \
           find/misc.c find/operator.c find/option.c find/getdate.c
	cc $(CFLAGS) -include unistd.h -include libgen.h \
	    -Dunix2003_compat=0 \
	    -o $@ find/find.c find/function.c find/ls.c \
	    find/main.c find/misc.c find/operator.c find/option.c \
	    find/getdate.c
who/who: who/who.c
	cc $(CFLAGS) -o $@ who/who.c

# --- iter 5 ---
# locate.bigram: single source; uses FreeBSD's <capsicum_helpers.h>
# (inline caph_*() — no extra lib). Needs the parent locate.h.
locate/bigram/locate.bigram: locate/bigram/locate.bigram.c
	cc $(CFLAGS) -I locate/locate -o $@ locate/bigram/locate.bigram.c
# locate.code: single source; no Apple/FreeBSD divergence. Same -I.
locate/code/locate.code: locate/code/locate.code.c
	cc $(CFLAGS) -I locate/locate -o $@ locate/code/locate.code.c
# locate: locate.c #includes fastfind.c inline (single TU), so only
# locate.c + util.c are linked. -I . because pathnames.h sits beside.
locate/locate/locate: locate/locate/locate.c locate/locate/util.c \
                      locate/locate/fastfind.c
	cc $(CFLAGS) -I locate/locate -o $@ \
	    locate/locate/locate.c locate/locate/util.c
# lastcomm: single source; libm for floor/fmod over acct(2) deltas.
lastcomm/lastcomm: lastcomm/lastcomm.c
	cc $(CFLAGS) -o $@ lastcomm/lastcomm.c -lm

install: all
	mkdir -p $(DESTDIR)/bin $(DESTDIR)/usr/bin
	# iter 1
	install -m 0555 true/true $(DESTDIR)/usr/bin/true
	install -m 0555 false/false $(DESTDIR)/usr/bin/false
	install -m 0555 echo/echo $(DESTDIR)/bin/echo
	install -m 0555 sleep/sleep $(DESTDIR)/bin/sleep
	install -m 0555 basename/basename $(DESTDIR)/usr/bin/basename
	# iter 2 — install at Apple-canonical paths (matches macOS):
	install -m 0555 apply/apply $(DESTDIR)/usr/bin/apply
	install -m 0555 dirname/dirname $(DESTDIR)/usr/bin/dirname
	install -m 0555 env/env $(DESTDIR)/usr/bin/env
	install -m 0555 getopt/getopt $(DESTDIR)/usr/bin/getopt
	install -m 0555 hostname/hostname $(DESTDIR)/bin/hostname
	install -m 0555 jot/jot $(DESTDIR)/usr/bin/jot
	install -m 0555 kill/kill $(DESTDIR)/bin/kill
	install -m 0555 logname/logname $(DESTDIR)/usr/bin/logname
	install -m 0555 mktemp/mktemp $(DESTDIR)/usr/bin/mktemp
	install -m 0555 nice/nice $(DESTDIR)/usr/bin/nice
	install -m 0555 nohup/nohup $(DESTDIR)/usr/bin/nohup
	install -m 0555 printenv/printenv $(DESTDIR)/usr/bin/printenv
	install -m 0555 printf/printf $(DESTDIR)/usr/bin/printf
	install -m 0555 pwd/pwd $(DESTDIR)/bin/pwd
	install -m 0555 realpath/realpath $(DESTDIR)/bin/realpath
	install -m 0555 renice/renice $(DESTDIR)/usr/bin/renice
	install -m 0555 tee/tee $(DESTDIR)/usr/bin/tee
	install -m 0555 uname/uname $(DESTDIR)/usr/bin/uname
	install -m 0555 what/what $(DESTDIR)/usr/bin/what
	install -m 0555 yes/yes $(DESTDIR)/usr/bin/yes
	# iter 3
	mkdir -p $(DESTDIR)/usr/sbin
	install -m 0555 chroot/chroot $(DESTDIR)/usr/sbin/chroot
	install -m 0555 date/date $(DESTDIR)/bin/date
	install -m 0555 hexdump/hexdump $(DESTDIR)/usr/bin/hexdump
	ln -f $(DESTDIR)/usr/bin/hexdump $(DESTDIR)/usr/bin/od
	install -m 0555 lockf/lockf $(DESTDIR)/usr/bin/lockf
	install -m 0555 script/script $(DESTDIR)/usr/bin/script
	install -m 0555 shlock/shlock $(DESTDIR)/usr/bin/shlock
	install -m 0555 stdbuf/stdbuf $(DESTDIR)/usr/bin/stdbuf
	install -m 0555 test/test $(DESTDIR)/bin/test
	ln -f $(DESTDIR)/bin/test $(DESTDIR)/bin/[
	install -m 0555 whereis/whereis $(DESTDIR)/usr/bin/whereis
	install -m 0555 which/which $(DESTDIR)/usr/bin/which
	install -m 0555 xargs/xargs $(DESTDIR)/usr/bin/xargs
	# iter 4
	install -m 0555 find/find $(DESTDIR)/usr/bin/find
	install -m 0555 who/who $(DESTDIR)/usr/bin/who
	# iter 5
	mkdir -p $(DESTDIR)/usr/libexec $(DESTDIR)/etc
	install -m 0555 locate/locate/locate $(DESTDIR)/usr/bin/locate
	install -m 0555 locate/bigram/locate.bigram \
	    $(DESTDIR)/usr/libexec/locate.bigram
	install -m 0555 locate/code/locate.code \
	    $(DESTDIR)/usr/libexec/locate.code
	install -m 0555 locate/locate/updatedb.sh \
	    $(DESTDIR)/usr/libexec/locate.updatedb
	install -m 0555 locate/locate/concatdb.sh \
	    $(DESTDIR)/usr/libexec/locate.concatdb
	install -m 0555 locate/locate/mklocatedb.sh \
	    $(DESTDIR)/usr/libexec/locate.mklocatedb
	install -m 0644 locate/locate/locate.rc $(DESTDIR)/etc/locate.rc
	install -m 0555 lastcomm/lastcomm $(DESTDIR)/usr/bin/lastcomm

clean:
	rm -f $(ITER1_BINS) $(ITER2_BINS) $(ITER3_BINS) $(ITER4_BINS) $(ITER5_BINS)

.PHONY: all clean install
