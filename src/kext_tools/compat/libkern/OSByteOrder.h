/*
 * libkern/OSByteOrder.h (NextBSD compat, nextbsd#182)
 * Minimal: map OSSwap to FreeBSD bswap. Used by the Mach-O byte-swap paths
 * (macho_util/fat_util), which are bypassed at runtime (ELF kexts).
 */
#ifndef _NEXTBSD_COMPAT_OSBYTEORDER_H
#define _NEXTBSD_COMPAT_OSBYTEORDER_H
#include <stdint.h>
#include <sys/endian.h>
#define OSSwapInt16(x) bswap16(x)
#define OSSwapInt32(x) bswap32(x)
#define OSSwapInt64(x) bswap64(x)
#define OSSwapConstInt16(x) bswap16(x)
#define OSSwapConstInt32(x) bswap32(x)
#define OSSwapConstInt64(x) bswap64(x)
#define OSSwapBigToHostInt16(x) be16toh(x)
#define OSSwapBigToHostInt32(x) be32toh(x)
#define OSSwapBigToHostInt64(x) be64toh(x)
#define OSSwapHostToBigInt16(x) htobe16(x)
#define OSSwapHostToBigInt32(x) htobe32(x)
#define OSSwapHostToBigInt64(x) htobe64(x)
#define OSSwapLittleToHostInt16(x) le16toh(x)
#define OSSwapLittleToHostInt32(x) le32toh(x)
#define OSSwapLittleToHostInt64(x) le64toh(x)
#define OSSwapHostToLittleInt16(x) htole16(x)
#define OSSwapHostToLittleInt32(x) htole32(x)
#define OSSwapHostToLittleInt64(x) htole64(x)
#endif
