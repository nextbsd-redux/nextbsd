/*
 * IOKitMatching.c — libIOKit iter 2: properties + matching.
 *
 * Translates hwregd's nvlist-shaped property bag into CFDictionary
 * (hwregd emits only NV_TYPE_STRING and NV_TYPE_NUMBER — see
 * src/hwregd/hwregd.c hwreg_get_properties) and Apple-style
 * IOService matching dictionaries into hwreg_lookup criteria. The
 * understood matching keys are IOProviderClass / IOClass (→
 * hwregd's `class`) and IONameMatch (→ `name`); other Apple keys
 * (IOPropertyMatch, IOBSDName, IOService plane parents/children,
 * regex) are silently ignored and the lookup falls back to whatever
 * criteria survived.
 */
#include <IOKit/IOKitLib.h>
#include "IOKitInternal.h"

#include "hwreg.h"
#include "hwreg_mig_types.h"
#include "nv.h"			/* libxpc nvlist API */

#include <CoreFoundation/CoreFoundation.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Build a CFMutableDictionary from a hwregd property nvlist.
 * Unknown nvlist entry types are silently skipped; hwregd's bag
 * never carries them today but keeping the switch permissive lets
 * later enrichment add types without breaking old clients.
 */
static CFMutableDictionaryRef
nvlist_to_cfdict(const nvlist_t *nv, CFAllocatorRef allocator)
{
	CFMutableDictionaryRef d;
	void *cookie = NULL;
	const char *key;
	int type;

	d = CFDictionaryCreateMutable(allocator, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (d == NULL)
		return (NULL);

	while ((key = nvlist_next(nv, &type, &cookie)) != NULL) {
		CFStringRef ck = CFStringCreateWithCString(allocator, key,
		    kCFStringEncodingUTF8);

		if (ck == NULL)
			continue;
		switch (type) {
		case NV_TYPE_STRING: {
			const char *s = nvlist_get_string(nv, key);
			CFStringRef cv = CFStringCreateWithCString(allocator,
			    s != NULL ? s : "", kCFStringEncodingUTF8);

			if (cv != NULL) {
				CFDictionarySetValue(d, ck, cv);
				CFRelease(cv);
			}
			break;
		}
		case NV_TYPE_NUMBER: {
			/* hwregd packs the unsigned PCI/id/state fields
			 * as nvlist NUMBERs (uint64_t). CFNumber speaks
			 * signed; cast through int64_t — every hwregd
			 * value fits the positive half. */
			uint64_t n = nvlist_get_number(nv, key);
			int64_t s = (int64_t)n;
			CFNumberRef cv = CFNumberCreate(allocator,
			    kCFNumberSInt64Type, &s);

			if (cv != NULL) {
				CFDictionarySetValue(d, ck, cv);
				CFRelease(cv);
			}
			break;
		}
		default:
			/* unknown type — skip */
			break;
		}
		CFRelease(ck);
	}
	return (d);
}

kern_return_t
IORegistryEntryCreateCFProperties(io_registry_entry_t entry,
    CFMutableDictionaryRef *properties, CFAllocatorRef allocator,
    IOOptionBits options __unused)
{
	mach_port_t svc = __io_hwregd_port();
	hwreg_blob_t blob;
	mach_msg_type_number_t cnt = 0;
	nvlist_t *nv;
	kern_return_t kr;

	if (entry == NULL || entry->kind != IOOBJ_KIND_ENTRY ||
	    properties == NULL)
		return (KERN_INVALID_ARGUMENT);
	if (svc == MACH_PORT_NULL)
		return (kIOReturnNoDevice);

	kr = hwreg_get_properties(svc, entry->node_id, blob, &cnt);
	if (kr != KERN_SUCCESS)
		return (kr);

	nv = nvlist_unpack(blob, cnt);
	if (nv == NULL)
		return (kIOReturnError);
	*properties = nvlist_to_cfdict(nv, allocator);
	nvlist_destroy(nv);
	return (*properties != NULL ? KERN_SUCCESS : kIOReturnError);
}

CFTypeRef
IORegistryEntryCreateCFProperty(io_registry_entry_t entry,
    CFStringRef key, CFAllocatorRef allocator, IOOptionBits options)
{
	CFMutableDictionaryRef dict = NULL;
	CFTypeRef val;

	if (IORegistryEntryCreateCFProperties(entry, &dict, allocator,
	    options) != KERN_SUCCESS || dict == NULL)
		return (NULL);
	val = CFDictionaryGetValue(dict, key);
	if (val != NULL)
		CFRetain(val);	/* Get returns +0; the contract here is +1 */
	CFRelease(dict);
	return (val);
}

kern_return_t
IOObjectGetClass(io_object_t object, io_name_t className)
{
	mach_port_t svc = __io_hwregd_port();
	uint64_t parent_id;
	int state;
	char name[32], classname[32], driver[32], path[256];
	kern_return_t kr;

	if (object == NULL || object->kind != IOOBJ_KIND_ENTRY ||
	    className == NULL)
		return (KERN_INVALID_ARGUMENT);
	if (svc == MACH_PORT_NULL)
		return (kIOReturnNoDevice);
	kr = hwreg_get_node(svc, object->node_id, &parent_id, &state,
	    name, classname, driver, path);
	if (kr != KERN_SUCCESS)
		return (kr);
	(void)strlcpy(className, classname, sizeof(io_name_t));
	return (KERN_SUCCESS);
}

CFMutableDictionaryRef
IOServiceMatching(const char *name)
{
	CFMutableDictionaryRef d;
	CFStringRef v;

	if (name == NULL)
		return (NULL);
	d = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	if (d == NULL)
		return (NULL);
	v = CFStringCreateWithCString(kCFAllocatorDefault, name,
	    kCFStringEncodingUTF8);
	if (v == NULL) {
		CFRelease(d);
		return (NULL);
	}
	CFDictionarySetValue(d, CFSTR(kIOProviderClassKey), v);
	CFRelease(v);
	return (d);
}

/*
 * Extract `key` from `matching` as a UTF-8 C-string, copied into
 * `buf`. Returns true if a string value was found and fit.
 */
static bool
copy_string_value(CFDictionaryRef matching, CFStringRef key,
    char *buf, size_t bufsz)
{
	CFTypeRef v = CFDictionaryGetValue(matching, key);

	if (v == NULL || CFGetTypeID(v) != CFStringGetTypeID())
		return (false);
	return (CFStringGetCString((CFStringRef)v, buf, (CFIndex)bufsz,
	    kCFStringEncodingUTF8) == TRUE);
}

/*
 * Translate `matching` to a hwreg_lookup criteria nvlist, pack it,
 * fire the RPC, return the malloc'd id array + count. Caller frees
 * `ids_out`. KERN_SUCCESS even when 0 matches — the result is
 * "no matches", not an error.
 */
static kern_return_t
lookup_matches(CFDictionaryRef matching, uint64_t **ids_out,
    uint32_t *count_out)
{
	mach_port_t svc = __io_hwregd_port();
	nvlist_t *crit;
	void *packed = NULL;
	size_t psz = 0;
	hwreg_blob_t critblob;
	uint64_t *ids;
	mach_msg_type_number_t nids = 128;
	char buf[64];
	kern_return_t kr;

	if (svc == MACH_PORT_NULL)
		return (kIOReturnNoDevice);
	crit = nvlist_create_dictionary(0);
	if (crit == NULL)
		return (kIOReturnError);

	if (copy_string_value(matching, CFSTR(kIOProviderClassKey),
	    buf, sizeof(buf)))
		nvlist_add_string(crit, "class", buf);
	else if (copy_string_value(matching, CFSTR(kIOClassKey),
	    buf, sizeof(buf)))
		nvlist_add_string(crit, "class", buf);
	if (copy_string_value(matching, CFSTR(kIONameMatchKey),
	    buf, sizeof(buf)))
		nvlist_add_string(crit, "name", buf);

	packed = nvlist_pack(crit, &psz);
	nvlist_destroy(crit);
	if (packed == NULL || psz > sizeof(critblob)) {
		free(packed);
		return (kIOReturnError);
	}
	(void)memcpy(critblob, packed, psz);
	free(packed);

	ids = calloc(nids, sizeof(*ids));
	if (ids == NULL)
		return (KERN_RESOURCE_SHORTAGE);
	kr = hwreg_lookup(svc, critblob, (mach_msg_type_number_t)psz,
	    ids, &nids);
	if (kr != KERN_SUCCESS) {
		free(ids);
		return (kr);
	}
	*ids_out = ids;
	*count_out = nids;
	return (KERN_SUCCESS);
}

io_service_t
IOServiceGetMatchingService(mach_port_t mainPort __unused,
    CFDictionaryRef matching)
{
	uint64_t *ids = NULL;
	uint32_t count = 0;
	io_service_t result = IO_OBJECT_NULL;

	if (matching == NULL)
		return (IO_OBJECT_NULL);
	if (lookup_matches(matching, &ids, &count) == KERN_SUCCESS &&
	    count > 0)
		result = __io_alloc_entry(ids[0]);
	free(ids);
	CFRelease(matching);	/* Apple contract: one reference consumed */
	return (result);
}

kern_return_t
IOServiceGetMatchingServices(mach_port_t mainPort __unused,
    CFDictionaryRef matching, io_iterator_t *iterator)
{
	uint64_t *ids = NULL;
	uint32_t count = 0;
	kern_return_t kr;

	if (iterator == NULL) {
		if (matching != NULL)
			CFRelease(matching);
		return (KERN_INVALID_ARGUMENT);
	}
	if (matching == NULL)
		return (KERN_INVALID_ARGUMENT);
	kr = lookup_matches(matching, &ids, &count);
	CFRelease(matching);	/* Apple contract: one reference consumed */
	if (kr != KERN_SUCCESS) {
		free(ids);
		return (kr);
	}
	*iterator = __io_alloc_iterator(ids, count);
	return (*iterator != IO_OBJECT_NULL ? KERN_SUCCESS
	    : KERN_RESOURCE_SHORTAGE);
}
