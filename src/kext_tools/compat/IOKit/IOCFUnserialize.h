/* IOKit/IOCFUnserialize.h — NextBSD compat decl (impl in libIOKit). (#182) */
#ifndef _NEXTBSD_COMPAT_IOCFUNSERIALIZE_H
#define _NEXTBSD_COMPAT_IOCFUNSERIALIZE_H
#include <CoreFoundation/CoreFoundation.h>
CFTypeRef IOCFUnserialize(const char *buffer, CFAllocatorRef allocator,
    CFOptionFlags options, CFStringRef *errorString);
CFTypeRef IOCFUnserializeBinary(const char *buffer, size_t bufferSize,
    CFAllocatorRef allocator, CFOptionFlags options, CFStringRef *errorString);
#endif
