/* os/base.h — FreeBSD shim. Apple's base macros for API attributes
 * (OS_EXPORT, OS_NOTHROW, OS_NONNULL, etc.). Map to GCC attributes
 * where they exist; no-op otherwise. */
#ifndef _FREEBSD_SHIM_OS_BASE_H_
#define _FREEBSD_SHIM_OS_BASE_H_

#include <sys/cdefs.h>

#define OS_EXPORT		__attribute__((visibility("default")))
#define OS_NOTHROW		__attribute__((__nothrow__))
#define OS_NONNULL(...)		__attribute__((__nonnull__(__VA_ARGS__)))
#define OS_NONNULL_ALL		__attribute__((__nonnull__))
#define OS_NORETURN		__attribute__((__noreturn__))
#define OS_INLINE		static __inline__
#define OS_ALWAYS_INLINE	__attribute__((__always_inline__))
#define OS_FORMAT_PRINTF(a,b)	__attribute__((__format__(__printf__, a, b)))
#define OS_WARN_RESULT		__attribute__((__warn_unused_result__))
#define OS_MALLOC		__attribute__((__malloc__))
#define OS_DEPRECATED(msg)	__attribute__((__deprecated__(msg)))
#define OS_DEPRECATED_REPLACE_WITH(repl) __attribute__((__deprecated__))
#define OS_REFINED_FOR_SWIFT
#define OS_SWIFT_NAME(_)
#define OS_SWIFT_UNAVAILABLE(msg)
#define OS_OBJECT_DECL(_)
#define OS_NOESCAPE		/* clang attribute __noescape — drop on GCC */

#define OS_ENUM(_name, _type, ...) typedef enum : _type { __VA_ARGS__ } _name##_t
#define OS_CLOSED_ENUM(_name, _type, ...) typedef enum : _type { __VA_ARGS__ } _name##_t
#define OS_OPTIONS(_name, _type, ...) typedef enum : _type { __VA_ARGS__ } _name##_t
#define OS_CLOSED_OPTIONS(_name, _type, ...) typedef enum : _type { __VA_ARGS__ } _name##_t

/* Compatibility names for older Apple code */
#define __OS_AVAILABILITY(...)
#define __OS_AVAILABILITY_MSG(...)

#endif
