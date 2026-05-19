/*
 * mach/vm_param.h — Apple-canonical VM page-size accessors.
 *
 * Apple's <mach/vm_param.h> exposes `vm_page_size`/`vm_page_mask` as
 * extern variables the kernel fills at process startup, and
 * `mach_vm_trunc_page` / `mach_vm_round_page` as macros over them.
 * libdispatch's mach.c (701-724) uses them to size receive buffers
 * to a page boundary.
 *
 * On FreeBSD we don't carry the Apple-private variables. CI runs on
 * FreeBSD/amd64 where PAGE_SIZE is 4096; future arm64 ports will
 * need to revisit the constants here (FreeBSD-arm64 uses 4 KiB pages
 * by default but ARMv8 supports 16 KiB; macOS uses 16 KiB). For now
 * a compile-time literal is enough — libdispatch's only consumers
 * size receive buffers and tolerate over-allocation.
 *
 * Return-type note. The macros cast to `__typeof__(x)` so they
 * preserve the argument's type at the call site — libdispatch
 * passes mach_msg_size_t (uint32_t) and assigns the result back to
 * mach_msg_size_t, which under -Wsign-conversion would otherwise
 * warn on a bare unsigned-long return.
 */
#ifndef _MACH_VM_PARAM_H_
#define _MACH_VM_PARAM_H_

#ifdef __cplusplus
extern "C" {
#endif

/*
 * FreeBSD amd64 page size. ARM64 / other archs will need a runtime
 * sysconf-backed accessor when ported.
 */
#ifndef _MACH_VM_PAGE_SIZE_DEFINED
#define _MACH_VM_PAGE_SIZE_DEFINED
#define vm_page_size		((unsigned long)4096)
#define vm_page_mask		((unsigned long)(vm_page_size - 1))
#endif

#define mach_vm_trunc_page(x)	((__typeof__(x))( \
		((unsigned long)(x)) & ~vm_page_mask))
#define mach_vm_round_page(x)	((__typeof__(x))( \
		(((unsigned long)(x)) + vm_page_mask) & ~vm_page_mask))

#ifdef __cplusplus
}
#endif

#endif /* !_MACH_VM_PARAM_H_ */
