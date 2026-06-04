# Out-of-tree build of mach.ko.
#
# Source lineage:
#   - sources under src/ derive from ravynOS legacy FreeBSD branch
#     sys/compat/mach/ (OSF / CMU / BSD 2-Clause), itself a NextBSD-era
#     port of XNU's osfmk/ipc/
#   - kernel-internal Mach headers under include/sys/mach/ come from
#     ravynOS sys/sys/mach/
#   - Apple-ism userland-shape headers under include/apple/ come from
#     ravynOS include/apple/
#
# This Makefile follows FreeBSD's standard out-of-tree kmod pattern. It
# does NOT require a built kernel by default — the opt_*.h headers
# shipped alongside this Makefile match a GENERIC kernel configuration.
#
# Users on custom kernel configs (anything that differs from GENERIC on
# CAPABILITIES, PPS_SYNC, COMPAT_MACH) should rebuild this module
# against their kernel build directory:
#
#     make KERNBUILDDIR=/usr/obj/usr/src/sys/MYKERNEL
#
# bsd.kmod.mk honors KERNBUILDDIR natively and will pick up the actual
# opt_*.h files generated from that kernel's config, overriding our
# shipped defaults. No source changes; no #ifdef wallpaper.
#
# SYSDIR points at the kernel source tree (default /usr/src/sys). The
# tree must be available (install src.txz on FreeBSD installs that don't
# have it). End users consuming the pre-built mach.ko tarball don't
# need /usr/src/sys/ — only people building from source do.

SYSDIR?=	/usr/src/sys

.PATH:	${.CURDIR}/src
.PATH:	${.CURDIR}/src/ipc
.PATH:	${.CURDIR}/src/kern

KMOD=	mach

DEBUG_FLAGS+= -g -O0

.if .defined(X_COMPILER_TYPE) && ${X_COMPILER_TYPE} == gcc
WERROR=
.endif

# Option headers. By default the ones shipped in this directory are used
# (matching GENERIC). If KERNBUILDDIR is set, bsd.kmod.mk uses the
# kernel build's actual opt_*.h instead. See README for details.
SRCS=	bus_if.h device_if.h vnode_if.h
SRCS+=	opt_compat_mach.h opt_ntp.h opt_capsicum.h opt_audit.h

# MIG-generated server stubs (pre-checked in; no mig invocation at build).
SRCS+=	host_priv_server.c mach_host_server.c mach_port_server.c mach_vm_server.c
SRCS+=	task_server.c vm_map_server.c clock_server.c

# compat/mach
SRCS+=	proc_info.c mach_clock.c mach_convert.c mach_host.c mach_host_priv.c
SRCS+=	mach_misc.c mach_module.c mach_processor.c mach_semaphore.c
SRCS+=	mach_task.c mach_thread.c mach_traps.c mach_vm.c
SRCS+=	mach_event_bridge.c

# compat/mach/ipc
SRCS+=	ipc_entry.c ipc_hash.c ipc_init.c ipc_kmsg.c ipc_kobject.c ipc_mqueue.c ipc_notify.c
SRCS+=	ipc_object.c ipc_port.c ipc_pset.c ipc_right.c ipc_space.c ipc_table.c ipc_thread.c
SRCS+=	mach_debug.c mach_msg.c mach_port.c

# compat/mach/kern
SRCS+=	ipc_tt.c ipc_host.c task.c thread_pool.c

# Out-of-tree compat stubs for Mach functions whose authoritative
# implementations live in Apple's osfmk/vm/vm_user.c — not in ravynOS's
# compat/mach/ port. See src/compat_stubs.c for rationale.
SRCS+=	compat_stubs.c

# Out-of-tree in-kernel Mach IPC round-trip test. Exposes
# `sysctl mach.test_roundtrip` which exercises ipc_port_alloc_special
# + ipc_kmsg_send + ipc_mqueue_receive end-to-end in kernel context.
# Phase B / Tier 1 task A. Drop this line to ship without the tester.
SRCS+=	mach_test.c

# Out-of-tree syscall wiring. ravynOS's mach_module.c uses
# syscall_helper_register on an array whose first entry has
# SYS_<name>=NO_SYSCALL, which is also the helper's end-of-array
# sentinel — so the loop exits immediately and no syscalls register.
# This file calls syscall_register directly per Mach syscall to
# work around that. Phase B / Tier 1 task B starts with just
# sys_mach_reply_port.
SRCS+=	mach_syscall_wire.c

# Out-of-tree read-only stats counters: mach.stats.ports_in_use and
# mach.stats.kmsgs_in_use. Phase B / Tier 1 task C.
SRCS+=	mach_stats.c

# Out-of-tree bus-quiescence busy counter + mach_wait_quiet syscall,
# built on the NEXTBSD kernel's device_match_start/device_match_end
# eventhandler pair. Exposes mach.bus.busy / mach.bus.quiesce_gen and
# the mach_wait_quiet syscall (backs IOKitWaitQuiet / IOServiceWaitQuiet).
SRCS+=	mach_busystate.c

# Include paths:
#   include/apple/  — Apple-ism headers (proc_info.h, kern_event.h, ...)
#   include/       — top-level so '#include <sys/mach/...>' resolves to
#                   our shipped kernel-internal Mach headers
#   ${.CURDIR}     — opt_*.h are at the top level of mach_kmod/
CFLAGS+=	-I${.CURDIR}/include/apple
CFLAGS+=	-I${.CURDIR}/include
CFLAGS+=	-I${.CURDIR}
CFLAGS+=	-DCOMPAT_MACH

# MACH_INTERNAL: <sys/mach/message.h> gates some internal-only prototypes
# (mach_msg_receive_results, etc.) behind #if defined(MACH_INTERNAL).
# ravynOS source files individually #define this before their #includes,
# but our compat_shim.h force-includes _mach_sysproto.h which pulls
# message.h up front (without MACH_INTERNAL set), and the include guard
# then prevents the gated declarations from ever being parsed even when
# a later file #defines MACH_INTERNAL. Promote to CFLAGS so every TU
# sees it from the start.
CFLAGS+=	-DMACH_INTERNAL=	# bare-define form, matches in-source `#define MACH_INTERNAL`

# AUDIT: GENERIC sets `options AUDIT`, which produces opt_audit.h with
# #define AUDIT 1 in /usr/obj/.../<KERNCONF>/. Stock kmod builds against
# SRCS-listed opt_*.h DON'T force-include those — the .h entries just
# get stub-touched in OBJDIR. We set the define directly here to match
# what a GENERIC build would see; users with kernels that disable AUDIT
# can override with `make KERNBUILDDIR=...` and bsd.kmod.mk will pick
# their actual opt_audit.h up (which won't define AUDIT, undoing this).
CFLAGS+=	-DAUDIT

# Force-include the out-of-tree compat shim. Provides aliases for
# struct proc/thread fields ravynOS added (p_machdata -> p_emuldata,
# td_machdata -> td_emuldata) and stub declarations for Apple-shape
# syscall handlers. See include/compat_shim.h for rationale.
CFLAGS+=	-include ${.CURDIR}/include/compat_shim.h

# Mach defines ~50 Apple-shape syscall handlers (sys_mach_msg_trap,
# sys_task_for_pid, ...). ravynOS auto-generates struct *_args + function
# prototypes from its syscalls master file; we don't have that on stock
# FreeBSD. The handler bodies compile fine — they just lack the headers
# the compiler would normally see in an in-tree build. These two
# warnings flag exactly that situation; demote from error to warning so
# the kmod links. Sysent registration is a separate Phase-B follow-up;
# until then no caller exists for these handlers, so the missing-
# prototype / visibility complaints are moot.
CFLAGS+=	-Wno-error=missing-prototypes
CFLAGS+=	-Wno-error=visibility

.include <bsd.kmod.mk>
