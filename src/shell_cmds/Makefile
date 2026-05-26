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
# Iter 1 scope: 5 trivial leaf tools (true, false, echo, sleep,
# basename) — all single-source pure POSIX. Same validation shape
# as file_cmds iter 1: prove the per-Apple-repo vendor + Makefile
# pattern scales beyond file_cmds before tackling the load-bearing
# tools (/bin/sh swap, id with libopendirectory shim, killall with
# KERN_PROCARGS2, su with BSM stub, the ~40 other tools).
#
# Iter 2+ (future): basename, chroot, date, dirname, env, expr,
# find, getopt, hexdump, hostname, jot, kill, lockf, logname,
# mktemp, nice, nohup, printenv, printf, pwd, realpath, renice,
# script, seq, sh, shlock, stdbuf, tee, test, time, tty, uname, w,
# who, xargs, yes, plus id/su/path_helper/killall with their shims.
#
# Usage: make -C src/shell_cmds install DESTDIR=$WORK/rootfs

DESTDIR ?=
CFLAGS = -O2 -pipe

ITER1_BINS = true/true false/false echo/echo sleep/sleep \
             basename/basename

all: $(ITER1_BINS)

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

install: all
	mkdir -p $(DESTDIR)/bin $(DESTDIR)/usr/bin
	install -m 0555 true/true $(DESTDIR)/usr/bin/true
	install -m 0555 false/false $(DESTDIR)/usr/bin/false
	install -m 0555 echo/echo $(DESTDIR)/bin/echo
	install -m 0555 sleep/sleep $(DESTDIR)/bin/sleep
	install -m 0555 basename/basename $(DESTDIR)/usr/bin/basename

clean:
	rm -f $(ITER1_BINS)

.PHONY: all clean install
