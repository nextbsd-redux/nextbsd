/* mach-o/swap.h — NextBSD compat stub (#182). macho_util's Mach-O byte-swap;
 * unused on NextBSD (kext executables are ELF .ko). Decls only. */
#ifndef _NEXTBSD_COMPAT_MACHO_SWAP_H
#define _NEXTBSD_COMPAT_MACHO_SWAP_H
#include <stdint.h>
enum NXByteOrder { NX_UnknownByteOrder, NX_LittleEndian, NX_BigEndian };
extern enum NXByteOrder NXHostByteOrder(void);
#endif
