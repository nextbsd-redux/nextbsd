/*
 * TargetConditionals.h — FreeBSD shim for Apple's <TargetConditionals.h>.
 *
 * Apple's TargetConditionals.h is a big matrix of TARGET_OS_* /
 * TARGET_CPU_* / TARGET_RT_* compile-time booleans the toolchain
 * fills in per build target. launchd-842 only consults a handful:
 * config.h keys HAVE_LIBAUDITD / HAVE_SYSTEMSTATS off
 * TARGET_OS_EMBEDDED, and core.c has a few TARGET_OS_EMBEDDED gates
 * for iOS-only jetsam paths.
 *
 * For our FreeBSD port the answer is fixed: we are NOT macOS, NOT
 * iOS, NOT a simulator. Everything Apple-platform-specific is 0.
 *
 * (Earlier this shim set TARGET_OS_MAC=1 / TARGET_OS_OSX=1 on the
 * theory that launchd's "real general-purpose OS" code paths key
 * off them. Verified 2026-05-15: launchd / liblaunch / launchctl
 * source has ZERO uses of TARGET_OS_MAC. The 1's were dead weight
 * for launchd but actively harmful for CF consumers -- CFBase.h
 * conditionally includes <libkern/OSTypes.h> and <MacTypes.h>
 * when TARGET_OS_OSX || TARGET_OS_MAC is true, and those headers
 * don't exist on FreeBSD. Setting both to 0 lets CFBase.h fall
 * through to its own typedef block for Boolean / UInt8 / SInt8 /
 * etc. CPU flags follow the compiler's predefines as before.)
 */
#ifndef _TARGETCONDITIONALS_H_SHIM_
#define _TARGETCONDITIONALS_H_SHIM_

/* --- OS family ---------------------------------------------------- */
#define TARGET_OS_MAC			0
#define TARGET_OS_OSX			0
#define TARGET_OS_WIN32			0
#define TARGET_OS_UNIX			1
#define TARGET_OS_EMBEDDED		0
#define TARGET_OS_IPHONE		0
#define TARGET_OS_IOS			0
#define TARGET_OS_TV			0
#define TARGET_OS_WATCH			0
#define TARGET_OS_BRIDGE		0
#define TARGET_OS_SIMULATOR		0
#define TARGET_IPHONE_SIMULATOR		0
#define TARGET_OS_DRIVERKIT		0

/* --- CPU family — follow the compiler's target predefines --------- */
#if defined(__x86_64__)
#define TARGET_CPU_X86			0
#define TARGET_CPU_X86_64		1
#define TARGET_CPU_ARM			0
#define TARGET_CPU_ARM64		0
#elif defined(__aarch64__)
#define TARGET_CPU_X86			0
#define TARGET_CPU_X86_64		0
#define TARGET_CPU_ARM			0
#define TARGET_CPU_ARM64		1
#elif defined(__i386__)
#define TARGET_CPU_X86			1
#define TARGET_CPU_X86_64		0
#define TARGET_CPU_ARM			0
#define TARGET_CPU_ARM64		0
#elif defined(__arm__)
#define TARGET_CPU_X86			0
#define TARGET_CPU_X86_64		0
#define TARGET_CPU_ARM			1
#define TARGET_CPU_ARM64		0
#else
#define TARGET_CPU_X86			0
#define TARGET_CPU_X86_64		0
#define TARGET_CPU_ARM			0
#define TARGET_CPU_ARM64		0
#endif
#define TARGET_CPU_PPC			0
#define TARGET_CPU_PPC64		0

/* --- Runtime / endianness ----------------------------------------- */
#define TARGET_RT_MAC_CFM		0
#define TARGET_RT_MAC_MACHO		0	/* ELF, not Mach-O */
#define TARGET_RT_LITTLE_ENDIAN		(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define TARGET_RT_BIG_ENDIAN		(__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define TARGET_RT_64_BIT		(defined(__LP64__))

#endif /* !_TARGETCONDITIONALS_H_SHIM_ */
