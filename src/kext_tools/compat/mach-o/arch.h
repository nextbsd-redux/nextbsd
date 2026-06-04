/* mach-o/arch.h — NextBSD compat: NXArchInfo arch lookup used by macho_util.
 * Decls only (impl unused: NextBSD kext executables are ELF .ko, not Mach-O,
 * so the Mach-O/arch paths get bypassed in the re-backing). (#182) */
#ifndef _NEXTBSD_COMPAT_MACHO_ARCH_H
#define _NEXTBSD_COMPAT_MACHO_ARCH_H
#include <stdint.h>
#ifndef CPU_TYPE_T_DEFINED
#define CPU_TYPE_T_DEFINED
typedef int32_t cpu_type_t;
typedef int32_t cpu_subtype_t;
#endif
typedef struct {
    const char   *name;
    cpu_type_t    cputype;
    cpu_subtype_t cpusubtype;
    const char   *description;
} NXArchInfo;
extern const NXArchInfo *NXGetAllArchInfos(void);
extern const NXArchInfo *NXGetLocalArchInfo(void);
extern const NXArchInfo *NXGetArchInfoFromName(const char *name);
extern const NXArchInfo *NXGetArchInfoFromCpuType(cpu_type_t cputype, cpu_subtype_t cpusubtype);
#endif
