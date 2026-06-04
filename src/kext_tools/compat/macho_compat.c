/*
 * macho_compat.c — NextBSD compat (#182).
 *
 * Link-time backing for the Mach-O byte-swap + local-arch helpers that Apple's
 * vendored macho_util.c / fat_util.c reference. NextBSD kext executables are
 * ELF .ko, not Mach-O, and the host is little-endian, so:
 *
 *   - the swap_*() family is never invoked at runtime (it only fires for a
 *     Mach-O whose byte order differs from the host's); the no-op bodies just
 *     satisfy the linker. If real cross-endian Mach-O support is ever needed,
 *     these get real implementations.
 *   - NXGetLocalArchInfo() returns a minimal descriptor for the build host so
 *     fat_util's "find the slice for this arch" logic has something to compare
 *     against (moot for ELF, which is never fat).
 */
#include <stddef.h>
#include <string.h>
#include <mach-o/arch.h>
#include <mach-o/fat.h>
#include <mach-o/swap.h>

/* mach/machine.h cputype values (Apple); only the host's is needed. */
#ifndef CPU_TYPE_X86_64
#define CPU_TYPE_X86_64 ((cpu_type_t)(0x00000007 | CPU_ARCH_ABI64))
#endif
#ifndef CPU_TYPE_ARM64
#define CPU_TYPE_ARM64  ((cpu_type_t)(0x0000000c | CPU_ARCH_ABI64))
#endif

void swap_mach_header(struct mach_header *mh, enum NXByteOrder t)
{ (void)mh; (void)t; }
void swap_mach_header_64(struct mach_header_64 *mh, enum NXByteOrder t)
{ (void)mh; (void)t; }
void swap_load_command(struct load_command *lc, enum NXByteOrder t)
{ (void)lc; (void)t; }
void swap_segment_command(struct segment_command *sg, enum NXByteOrder t)
{ (void)sg; (void)t; }
void swap_segment_command_64(struct segment_command_64 *sg, enum NXByteOrder t)
{ (void)sg; (void)t; }

const NXArchInfo *
NXGetLocalArchInfo(void)
{
#if defined(__aarch64__) || defined(__arm64__)
    static const NXArchInfo local = {
        "arm64", CPU_TYPE_ARM64, 0, NX_LittleEndian, "NextBSD host (arm64)"
    };
#else
    static const NXArchInfo local = {
        "x86_64", CPU_TYPE_X86_64, 0, NX_LittleEndian, "NextBSD host (x86_64)"
    };
#endif
    return &local;
}

const NXArchInfo *
NXGetArchInfoFromName(const char *name)
{
    const NXArchInfo *local = NXGetLocalArchInfo();

    return (name != NULL && local != NULL && strcmp(name, local->name) == 0)
        ? local : NULL;
}

const NXArchInfo *
NXGetArchInfoFromCpuType(cpu_type_t cputype, cpu_subtype_t cpusubtype)
{
    const NXArchInfo *local = NXGetLocalArchInfo();

    (void)cpusubtype;
    /* Single-arch host: match the local arch (cputype 0 means "any"). */
    return (local != NULL && (cputype == 0 || cputype == local->cputype))
        ? local : NULL;
}

/* No fat (universal) binaries on NextBSD — kext executables are thin ELF .ko. */
struct fat_arch *
NXFindBestFatArch(cpu_type_t cputype, cpu_subtype_t cpusubtype,
    struct fat_arch *fat_archs, uint32_t nfat_archs)
{
    (void)cputype; (void)cpusubtype; (void)fat_archs; (void)nfat_archs;
    return NULL;
}

/* NextBSD does not gate kext loads on the OSBundleCopyright string format. */
int
kxld_validate_copyright_string(const char *str)
{
    (void)str;
    return 1;
}
