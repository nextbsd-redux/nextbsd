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

all: $(ITER1_BINS) $(ITER2_BINS)

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
	cc $(CFLAGS) -o $@ apply/apply.c
dirname/dirname: dirname/dirname.c
	cc $(CFLAGS) -o $@ dirname/dirname.c
# env: env.c + envopts.c (multi-source).
env/env: env/env.c env/envopts.c
	cc $(CFLAGS) -o $@ env/env.c env/envopts.c
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

clean:
	rm -f $(ITER1_BINS) $(ITER2_BINS)

.PHONY: all clean install
