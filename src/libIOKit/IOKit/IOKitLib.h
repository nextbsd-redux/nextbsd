/*
 * IOKit/IOKitLib.h — public header for the freebsd-launchd-mach
 * IOKit userland facade.
 *
 * This is the Apple IOKit C API as a thin client layered over
 * hwregd's MIG hwreg.defs Mach RPC. Per the plan
 * (pkgdemon.github.io/freebsd-hardware-registry-iokit-plan.html §5),
 * the facade is NOT a port of Apple's IOKitUser source (which
 * decomposes to a kernel device.defs surface this repo has no kernel
 * for) — it is a fresh, thin CF wrapper over hwregd. Apple's
 * io_object_t is a mach_port_t naming a kernel-resident IOService;
 * the facade has no such kernel, so io_object_t here is an opaque
 * pointer to a client-side struct holding a hwregd node id (or a
 * captured id array for iterators). Each routine that returns one
 * allocates it; the caller releases with IOObjectRelease.
 *
 * iter 1 — read-only registry walk:
 *   IORegistryGetRootEntry, IORegistryEntryGetChildIterator,
 *   IOIteratorNext, IORegistryEntryGetName, IORegistryEntryGetPath,
 *   IOObjectRetain, IOObjectRelease.
 *
 * iter 2 — properties + matching (CF-typed):
 *   IORegistryEntryCreateCFProperties, IORegistryEntryCreateCFProperty,
 *   IOObjectGetClass, IOServiceMatching, IOServiceGetMatchingService,
 *   IOServiceGetMatchingServices.
 *
 * Later iterations add the `ioreg` tool (iter 3) and notifications
 * (iter 4, K2 of the plan).
 *
 * NOTE: there is also a separate Apple-API stub at
 * src/launchd/freebsd-shims/IOKit/IOKitLib.h covering the four IOKit
 * calls launchctl makes (IOKitWaitQuiet etc.). That shim aliases
 * io_object_t to mach_port_t and is unrelated to this facade; they
 * sit on different include paths and are not used in the same TU.
 */
#ifndef _IOKIT_IOKITLIB_H_
#define _IOKIT_IOKITLIB_H_

#include <sys/cdefs.h>

#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_port.h>
#include <mach/kern_return.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Opaque client-side handle. See the header comment.
 */
typedef struct __IOObject *io_object_t;
typedef io_object_t io_registry_entry_t;
typedef io_object_t io_service_t;
typedef io_object_t io_iterator_t;

#define IO_OBJECT_NULL ((io_object_t)0)

/*
 * Apple's IOKit name + path buffer typedefs. io_name_t is a 128-byte
 * fixed array used for class / registry-entry names; io_string_t is
 * 512 bytes and carries plane-qualified paths.
 */
typedef char io_name_t[128];
typedef char io_string_t[512];

/*
 * IOOptionBits — Apple's IOKit options-mask typedef. Accepted by
 * several routines (CreateCFProperties etc.) for source
 * compatibility; iter 2 ignores it.
 */
typedef uint32_t IOOptionBits;

/*
 * IOReturn — Apple's IOKit error code space. kern_return_t fits the
 * same range. iter 1 only needs a small subset (the rest exist on
 * macOS); add more as later iters grow the API.
 */
typedef int IOReturn;
#define kIOReturnSuccess     0
#define kIOReturnError       0x2bc
#define kIOReturnNoDevice    0x2c0
#define kIOReturnBadArgument 0x2c9
#define kIOReturnNotFound    0x2f0

/*
 * "main port" sentinels. On macOS these select the IOKit master
 * server. The facade resolves org.freebsd.hwregd lazily via
 * bootstrap_look_up on first use; this argument is ignored, but
 * declared so the Apple signatures match.
 */
#define kIOMainPortDefault   ((mach_port_t)0)
#define kIOMasterPortDefault ((mach_port_t)0)	/* deprecated alias */

/*
 * IORegistryGetRootEntry — handle to the registry root (the newbus
 * root device hwregd snapshots at startup). Returns IO_OBJECT_NULL
 * if hwregd is unreachable or the registry is empty. The returned
 * handle must be released with IOObjectRelease.
 */
io_registry_entry_t	IORegistryGetRootEntry(mach_port_t mainPort);

/*
 * IORegistryEntryGetChildIterator — iterator over `entry`'s direct
 * children. `plane` is accepted for source compatibility and
 * ignored (this facade has one plane, IOService). The iterator
 * must be released with IOObjectRelease.
 */
kern_return_t	IORegistryEntryGetChildIterator(io_registry_entry_t entry,
		    const io_name_t plane, io_iterator_t *iterator);

/*
 * IOIteratorNext — next entry from `iterator`, or IO_OBJECT_NULL
 * when exhausted. Each returned entry must be released with
 * IOObjectRelease.
 */
io_object_t	IOIteratorNext(io_iterator_t iterator);

/*
 * IORegistryEntryGetName — `entry`'s short name (e.g. "em0") into
 * the caller's io_name_t buffer.
 */
kern_return_t	IORegistryEntryGetName(io_registry_entry_t entry,
		    io_name_t name);

/*
 * IORegistryEntryGetPath — `entry`'s plane-qualified path into the
 * caller's io_string_t buffer. The result is "IOService:" followed
 * by the entry's newbus-tree path (e.g. "IOService:/pci0/em0").
 * `plane` is ignored.
 */
kern_return_t	IORegistryEntryGetPath(io_registry_entry_t entry,
		    const io_name_t plane, io_string_t path);

/*
 * IOObjectRetain — gain an additional reference to a handle.
 * IOObjectRelease — drop a reference and free the handle at zero.
 * Releasing IO_OBJECT_NULL is a no-op (matches macOS).
 */
kern_return_t	IOObjectRetain(io_object_t object);
kern_return_t	IOObjectRelease(io_object_t object);

/*
 * iter 2 ----------------------------------------------------------
 *
 * Well-known IOService matching dictionary keys (the small subset
 * the facade understands). The full Apple set is much larger; the
 * facade translates IOProviderClass / IOClass to hwregd's `class`
 * criterion and IONameMatch to `name`.
 */
#define kIOProviderClassKey	"IOProviderClass"
#define kIOClassKey		"IOClass"
#define kIONameMatchKey		"IONameMatch"

/*
 * IORegistryEntryCreateCFProperties — `entry`'s full property bag
 * as a freshly-allocated CFMutableDictionary (keys = CFString,
 * values = CFString or CFNumber — the only types hwregd's bag
 * carries). The caller releases the dictionary with CFRelease.
 * `options` is ignored.
 */
kern_return_t	IORegistryEntryCreateCFProperties(io_registry_entry_t entry,
		    CFMutableDictionaryRef *properties,
		    CFAllocatorRef allocator, IOOptionBits options);

/*
 * IORegistryEntryCreateCFProperty — a single property's value, +1
 * retain. NULL if `entry` has no such property. `options` is
 * ignored.
 */
CFTypeRef	IORegistryEntryCreateCFProperty(io_registry_entry_t entry,
		    CFStringRef key, CFAllocatorRef allocator,
		    IOOptionBits options);

/*
 * IOObjectGetClass — the entry's class name (hwregd's `class`
 * field — "CPU" / "PCIDevice" / "USBDevice" / etc.) into the
 * caller's io_name_t buffer.
 */
kern_return_t	IOObjectGetClass(io_object_t object, io_name_t className);

/*
 * IOServiceMatching — build a fresh matching dictionary against a
 * provider class. Equivalent to:
 *   { kIOProviderClassKey: CFSTR(name) }
 * Caller owns the returned dictionary; IOServiceGetMatchingService*
 * consume one reference.
 */
CFMutableDictionaryRef	IOServiceMatching(const char *name);

/*
 * IOServiceGetMatchingService(s) — look up service(s) whose hwregd
 * node matches `matching`. The single-result form returns the first
 * match (or IO_OBJECT_NULL); the iterator form returns an iterator
 * over all matches (always non-NULL on success — an empty iterator
 * is valid and yields IO_OBJECT_NULL on first IOIteratorNext).
 *
 * BOTH forms consume one reference of `matching` (Apple contract:
 * "one reference is always consumed by this function") so the
 * idiom `IOServiceGetMatchingService(mp, IOServiceMatching("X"))`
 * does not leak.
 */
io_service_t	IOServiceGetMatchingService(mach_port_t mainPort,
		    CFDictionaryRef matching);

kern_return_t	IOServiceGetMatchingServices(mach_port_t mainPort,
		    CFDictionaryRef matching, io_iterator_t *iterator);

#ifdef __cplusplus
}
#endif

#endif /* _IOKIT_IOKITLIB_H_ */
