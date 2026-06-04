/* mach-o/arch.h — NextBSD compat: NXArchInfo arch lookup used by macho_util.
 * Decls only (impl unused: NextBSD kext executables are ELF .ko, not Mach-O,
 * so the Mach-O/arch paths get bypassed in the re-backing). (#182) */
#ifndef _NEXTBSD_COMPAT_MACHO_ARCH_H
#define _NEXTBSD_COMPAT_MACHO_ARCH_H
#include <stdint.h>
#include <architecture/byte_order.h>   /* enum NXByteOrder / NX_UnknownByteOrder */
#ifndef CPU_TYPE_T_DEFINED
#define CPU_TYPE_T_DEFINED
typedef int32_t cpu_type_t;
typedef int32_t cpu_subtype_t;
#endif
/* mach/machine.h ABI-width bit on cputype (Apple value), used by arch checks. */
#ifndef CPU_ARCH_ABI64
#define CPU_ARCH_ABI64 0x01000000
#endif
typedef struct {
    const char   *name;
    cpu_type_t    cputype;
    cpu_subtype_t cpusubtype;
    enum NXByteOrder byteorder;
    const char   *description;
} NXArchInfo;
extern const NXArchInfo *NXGetAllArchInfos(void);
extern const NXArchInfo *NXGetLocalArchInfo(void);
extern const NXArchInfo *NXGetArchInfoFromName(const char *name);
extern const NXArchInfo *NXGetArchInfoFromCpuType(cpu_type_t cputype, cpu_subtype_t cpusubtype);

/* fat_util.c uses NXFindBestFatArch; declare it (returns a pointer) so the
 * caller doesn't truncate it through an implicit int declaration on LP64. */
struct fat_arch;
extern struct fat_arch *NXFindBestFatArch(cpu_type_t cputype,
    cpu_subtype_t cpusubtype, struct fat_arch *fat_archs, uint32_t nfat_archs);
#endif
