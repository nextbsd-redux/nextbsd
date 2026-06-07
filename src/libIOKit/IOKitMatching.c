/*
 * IOKitMatching.c — libIOKit iter 2: properties + matching.
 *
 * Translates the kernel /dev/ioregistry nvlist-shaped property bag
 * (IOREGIOCPROPS, carrying NV_TYPE_STRING and NV_TYPE_NUMBER) into a
 * CFDictionary, and Apple-style IOService matching dictionaries into the
 * flat kernel `struct ioreg_criteria` used by IOREGIOC{LOOKUP,WATCH}. The
 * understood matching keys are IOProviderClass / IOClass (→ `classname`)
 * and IONameMatch (→ `name`); other Apple keys (IOPropertyMatch, IOBSDName,
 * IOService plane parents/children, regex) are silently ignored and the
 * lookup uses whatever criteria survived.
 */
#include <IOKit/IOKitLib.h>
#include "IOKitInternal.h"

#include "ioregistry.h"		/* vendored K1 ABI; canonical in nextbsd-kernel */

#include "nv.h"			/* libxpc nvlist API (IOREGIOCPROPS property bag) */

#include <CoreFoundation/CoreFoundation.h>

#include <sys/ioctl.h>		/* ioctl(2) on /dev/ioregistry */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Build a CFMutableDictionary from a kernel property nvlist.
 * Unknown nvlist entry types are silently skipped; the bag
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
			/* The kernel packs the unsigned PCI/id/state fields
			 * as nvlist NUMBERs (uint64_t). CFNumber speaks
			 * signed; cast through int64_t — every such
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

/*
 * Pull a node's packed-nvlist property bag from /dev/ioregistry
 * (IOREGIOCPROPS). Two-step: size the bag (NULL buf), allocate, then
 * fetch. Returns a malloc'd buffer (caller frees) and its length, or
 * NULL with *kr_out set on failure. Returns KERN_SUCCESS / sets *len_out
 * = 0 with a non-NULL allocation if the bag is empty (len 0 still gets a
 * 1-byte allocation so the caller frees a real pointer).
 */
static void *
ioreg_fetch_props(int fd, uint64_t node_id, uint32_t *len_out,
    kern_return_t *kr_out)
{
	struct ioreg_props pr;
	void *buf;
	uint32_t need;

	/* Step 1: size the bag (NULL buf). */
	(void)memset(&pr, 0, sizeof(pr));
	pr.id = node_id;
	pr.len = 0;
	pr.buf = 0;
	if (ioctl(fd, IOREGIOCPROPS, &pr) != 0) {
		*kr_out = kIOReturnNotFound;
		return (NULL);
	}
	need = pr.len;
	buf = malloc(need != 0 ? need : 1);
	if (buf == NULL) {
		*kr_out = KERN_RESOURCE_SHORTAGE;
		return (NULL);
	}
	if (need == 0) {		/* empty bag — nothing to fetch */
		*len_out = 0;
		*kr_out = KERN_SUCCESS;
		return (buf);
	}

	/* Step 2: fetch into the sized buffer. */
	(void)memset(&pr, 0, sizeof(pr));
	pr.id = node_id;
	pr.len = need;
	pr.buf = (uint64_t)(uintptr_t)buf;
	if (ioctl(fd, IOREGIOCPROPS, &pr) != 0 || pr.len > need) {
		/* A grown len means the bag changed under us — bail rather
		 * than unpack a truncated nvlist. */
		free(buf);
		*kr_out = kIOReturnError;
		return (NULL);
	}
	*len_out = pr.len;
	*kr_out = KERN_SUCCESS;
	return (buf);
}

kern_return_t
IORegistryEntryCreateCFProperties(io_registry_entry_t entry,
    CFMutableDictionaryRef *properties, CFAllocatorRef allocator,
    IOOptionBits options __unused)
{
	int fd;
	nvlist_t *nv;
	kern_return_t kr;

	if (entry == NULL || entry->kind != IOOBJ_KIND_ENTRY ||
	    properties == NULL)
		return (KERN_INVALID_ARGUMENT);

	fd = __io_ioregistry_fd();
	if (fd < 0)
		return (kIOReturnNoDevice);

	{
		void *buf;
		uint32_t len = 0;

		buf = ioreg_fetch_props(fd, entry->node_id, &len, &kr);
		if (buf == NULL)
			return (kr);
		if (len == 0) {
			/* Empty bag -> empty dictionary (still a success). */
			free(buf);
			*properties = CFDictionaryCreateMutable(allocator, 0,
			    &kCFTypeDictionaryKeyCallBacks,
			    &kCFTypeDictionaryValueCallBacks);
			return (*properties != NULL ? KERN_SUCCESS
			    : kIOReturnError);
		}
		nv = nvlist_unpack(buf, len);
		free(buf);
		if (nv == NULL)
			return (kIOReturnError);
		*properties = nvlist_to_cfdict(nv, allocator);
		nvlist_destroy(nv);
		return (*properties != NULL ? KERN_SUCCESS : kIOReturnError);
	}
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
	char classname[32];
	kern_return_t kr;

	if (object == NULL || object->kind != IOOBJ_KIND_ENTRY ||
	    className == NULL)
		return (KERN_INVALID_ARGUMENT);
	/* __io_node reads /dev/ioregistry (IOREGIOCNODE); only the class
	 * field is wanted, so the rest are skipped (NULLs). */
	kr = __io_node(object->node_id, NULL, NULL, NULL, classname, NULL,
	    NULL);
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

void
__io_extract_criteria(CFDictionaryRef matching, struct io_criteria *out)
{
	(void)memset(out, 0, sizeof(*out));
	if (matching == NULL)
		return;
	if (!copy_string_value(matching, CFSTR(kIOProviderClassKey),
	    out->klass, sizeof(out->klass)))
		(void)copy_string_value(matching, CFSTR(kIOClassKey),
		    out->klass, sizeof(out->klass));
	(void)copy_string_value(matching, CFSTR(kIONameMatchKey),
	    out->name, sizeof(out->name));
}

/*
 * Fill a flat kernel `struct ioreg_criteria` from an io_criteria (#218).
 *
 * The kernel /dev/ioregistry IOREGIOC{WATCH,LOOKUP} handlers read this fixed
 * by-value struct directly (no packed-nvlist criteria bag). That avoids the
 * libxpc-vs-libnv wire-format mismatch (libxpc's packer added an extra
 * nvlh_type header byte and omitted the trailing nvph_nitems the kernel's libnv
 * reader expects) which made every kernel nvlist_unpack() return NULL, so
 * IOREGIOCWATCH returned EINVAL and no watch ever registered — the #218
 * round-trip break. There is no serialization here, so nothing can mismatch.
 *
 * Mapping: io_criteria name/klass/driver -> the kernel criteria name/classname/
 * driver. Empty source strings are left zeroed (the kernel treats a zero field
 * as a wildcard). The pci_* numeric criteria are unused by the current Apple
 * matching keys and stay zero (also wildcard).
 */
void
__io_fill_criteria(const struct io_criteria *c, struct ioreg_criteria *out)
{
	(void)memset(out, 0, sizeof(*out));
	if (c->name[0] != '\0')
		(void)strlcpy(out->name, c->name, sizeof(out->name));
	if (c->klass[0] != '\0')
		(void)strlcpy(out->classname, c->klass, sizeof(out->classname));
	if (c->driver[0] != '\0')
		(void)strlcpy(out->driver, c->driver, sizeof(out->driver));
}

/*
 * Translate `matching` to a flat kernel criteria struct, fire IOREGIOCLOOKUP,
 * return the malloc'd id array + count. Caller frees `ids_out`. KERN_SUCCESS
 * even when 0 matches — the result is "no matches", not an error.
 */
#define IOKIT_MAX_MATCHES	128	/* fixed per-call id array bound */

static kern_return_t
lookup_matches(CFDictionaryRef matching, uint64_t **ids_out,
    uint32_t *count_out)
{
	int fd = __io_ioregistry_fd();
	struct io_criteria c;
	uint64_t *ids;
	uint32_t cap = IOKIT_MAX_MATCHES;

	if (fd < 0)
		return (kIOReturnNoDevice);

	__io_extract_criteria(matching, &c);
	ids = calloc(cap, sizeof(*ids));
	if (ids == NULL)
		return (KERN_RESOURCE_SHORTAGE);

	{
		struct ioreg_lookup lk;

		/* Flat by-value criteria (#218): no nvlist packing, so nothing to
		 * mismatch. An all-zero criteria (no key survived extraction)
		 * matches every live node — the kernel ABI's documented default. */
		(void)memset(&lk, 0, sizeof(lk));
		__io_fill_criteria(&c, &lk.criteria);
		lk.max = cap;
		lk.matches = (uint64_t)(uintptr_t)ids;
		if (ioctl(fd, IOREGIOCLOOKUP, &lk) != 0) {
			free(ids);
			return (kIOReturnError);
		}
		if (lk.count > cap)	/* truncated to our fixed array */
			lk.count = cap;
		*ids_out = ids;
		*count_out = lk.count;
		return (KERN_SUCCESS);
	}
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
