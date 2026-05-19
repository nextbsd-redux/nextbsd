# libsystem_kernel — userland C shim that turns the mach.ko-registered
# syscalls into named functions (mach_reply_port, mach_task_self, ...).
#
# Naming: matches Apple's libsystem_kernel.dylib on macOS so Apple-source
# code that links -lsystem_kernel works unchanged. See spike doc §3.
#
# Build:   cd src/libmach && make
# Install: make DESTDIR=/path/to/rootfs install
#   produces:
#     ${DESTDIR}/usr/lib/system/libsystem_kernel.so
#     ${DESTDIR}/usr/lib/system/libsystem_kernel.so.0
#     ${DESTDIR}/usr/include/mach/mach_traps.h
#     ${DESTDIR}/usr/include/mach/mach_traps_mux.h
#     ${DESTDIR}/usr/include/mach/mach_port.h
#     ${DESTDIR}/usr/include/mach/message.h
#     ${DESTDIR}/usr/include/mach/task_special_ports.h
#     ${DESTDIR}/usr/include/mach/host_special_ports.h
#     ${DESTDIR}/usr/libdata/pkgconfig/libsystem_kernel.pc

LIB=		system_kernel
SHLIB_MAJOR=	0
SRCS=		mach_traps.c \
		dispatch_kevent.c

CFLAGS+=	-I${.CURDIR}/include
NO_MAN=		yes
WARNS?=		6

# Task #39 update: <mach/mach.h> + <mach/clock.h> are now installed
# alongside the rest of the libmach headers. libdispatch's HAVE_MACH
# path (build.sh:380, -DHAVE_MACH:BOOL=ON) wants both: internal.h
# includes <mach/clock.h> and <mach/mach.h> unconditionally on the
# HAVE_MACH branch. Apple-internal types referenced from those
# headers (mach_voucher_t, etc.) are kept minimal or stubbed.
INCS=		include/mach/mach.h \
		include/mach/mach_traps.h \
		include/mach/mach_traps_mux.h \
		include/mach/mach_port.h \
		include/mach/message.h \
		include/mach/task_special_ports.h \
		include/mach/host_special_ports.h \
		include/mach/boolean.h \
		include/mach/kern_return.h \
		include/mach/std_types.h \
		include/mach/ndr.h \
		include/mach/machine.h \
		include/mach/vm_map.h \
		include/mach/vm_param.h \
		include/mach/error.h \
		include/mach/clock.h \
		include/mach/clock_types.h \
		include/mach/mach_vm.h \
		include/mach/sync_policy.h \
		include/mach/dispatch_kevent.h \
		include/mach/port.h \
		include/mach/notify.h \
		include/mach/mach_types.h \
		include/mach/mig_errors.h \
		include/mach/mig.h \
		include/mach/thread_status.h \
		include/mach/mach_error.h \
		include/mach/mach_interface.h \
		include/mach/host_info.h \
		include/mach/mach_host.h \
		include/mach/mach_time.h \
		include/mach/exception.h \
		include/mach/host_reboot.h \
		include/mach/task_policy.h \
		include/mach/std_types.defs \
		include/mach/mach_types.defs \
		include/mach/machine_types.defs
INCSDIR=	${PREFIX}/include/mach
LIBDIR=		${PREFIX}/lib/system

# pkg-config file: consumers do `pkg-config --libs libsystem_kernel`
# and get the right -L and -l flags for /usr/lib/system/.
FILES=		libsystem_kernel.pc
FILESDIR=	${PREFIX}/libdata/pkgconfig

PREFIX?=	/usr

.include <bsd.lib.mk>
