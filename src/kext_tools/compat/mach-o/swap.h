/* mach-o/swap.h — NextBSD compat (#182).
 *
 * Apple's macho_util.c byte-swaps Mach-O structures via this header's
 * swap_*() family. On NextBSD kext executables are ELF .ko (never Mach-O) and
 * the host is little-endian, so cross-endian Mach-O swapping never happens at
 * runtime — but macho_util.c must still link. We only declare the family here;
 * no-op definitions live in macho_compat.c.
 *
 * enum NXByteOrder + NXHostByteOrder() come from <architecture/byte_order.h>,
 * pulled transitively via <mach-o/loader.h> (do NOT redefine them here).
 */
#ifndef _NEXTBSD_COMPAT_MACHO_SWAP_H
#define _NEXTBSD_COMPAT_MACHO_SWAP_H
#include <stdint.h>
#include <mach-o/loader.h>   /* Mach-O struct decls + byte_order (NXByteOrder) */

void swap_mach_header(struct mach_header *mh, enum NXByteOrder target);
void swap_mach_header_64(struct mach_header_64 *mh, enum NXByteOrder target);
void swap_load_command(struct load_command *lc, enum NXByteOrder target);
void swap_segment_command(struct segment_command *sg, enum NXByteOrder target);
void swap_segment_command_64(struct segment_command_64 *sg, enum NXByteOrder target);

#endif
