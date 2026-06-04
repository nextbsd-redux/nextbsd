/* IOKit/IOCFSerialize.h — NextBSD compat decl (impl in libIOKit). (#182) */
#ifndef _NEXTBSD_COMPAT_IOCFSERIALIZE_H
#define _NEXTBSD_COMPAT_IOCFSERIALIZE_H
#include <CoreFoundation/CoreFoundation.h>
enum { kIOCFSerializeToBinary = 0x00000001U };
CFDataRef IOCFSerialize(CFTypeRef object, CFOptionFlags options);
#endif
