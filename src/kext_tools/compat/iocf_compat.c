/*
 * iocf_compat.c — NextBSD compat (#182).
 *
 * IOCFSerialize / IOCFUnserialize convert between CF objects and the
 * (XML) property-list wire format. On XNU they live in IOKitLib; the NextBSD
 * libIOKit (an hwregd facade) does not provide them, but OSKext needs them to
 * read Info.plists and its caches. They are exactly property-list
 * serialization, so back them with CoreFoundation's CFPropertyList API.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <string.h>

#include "IOKit/IOCFSerialize.h"
#include "IOKit/IOCFUnserialize.h"

CFDataRef
IOCFSerialize(CFTypeRef object, CFOptionFlags options)
{
	(void)options;	/* kIOCFSerializeToBinary unused: emit XML */
	if (object == NULL) {
		return (NULL);
	}
	return (CFPropertyListCreateData(kCFAllocatorDefault, object,
	    kCFPropertyListXMLFormat_v1_0, 0, NULL));
}

static CFTypeRef
unserialize_bytes(const UInt8 *bytes, CFIndex length, CFAllocatorRef allocator,
    CFStringRef *errorString)
{
	CFDataRef data;
	CFTypeRef obj;

	if (errorString != NULL) {
		*errorString = NULL;
	}
	if (bytes == NULL || length <= 0) {
		return (NULL);
	}
	data = CFDataCreate(allocator, bytes, length);
	if (data == NULL) {
		return (NULL);
	}
	obj = CFPropertyListCreateWithData(allocator, data,
	    kCFPropertyListImmutable, NULL, NULL);
	CFRelease(data);
	return (obj);
}

CFTypeRef
IOCFUnserialize(const char *buffer, CFAllocatorRef allocator,
    CFOptionFlags options, CFStringRef *errorString)
{
	(void)options;
	if (buffer == NULL) {
		return (NULL);
	}
	return (unserialize_bytes((const UInt8 *)buffer,
	    (CFIndex)strlen(buffer), allocator, errorString));
}

CFTypeRef
IOCFUnserializeBinary(const char *buffer, size_t bufferSize,
    CFAllocatorRef allocator, CFOptionFlags options, CFStringRef *errorString)
{
	(void)options;
	return (unserialize_bytes((const UInt8 *)buffer, (CFIndex)bufferSize,
	    allocator, errorString));
}
