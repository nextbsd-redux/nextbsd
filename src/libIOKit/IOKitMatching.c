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

#include "ioregistry.h"		/* vendored K1 ABI; canonical in nextbsd-kernel */

#include "hwreg.h"		/* fallback MIG stubs */
#include "hwreg_mig_types.h"
#include "nv.h"			/* libxpc nvlist API */

#include <CoreFoundation/CoreFoundation.h>

#include <sys/ioctl.h>		/* ioctl(2) on /dev/ioregistry */

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
	if (fd >= 0) {
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

	/* Fallback: hwregd MIG property bag. */
	{
		mach_port_t svc = __io_hwregd_port();
		hwreg_blob_t blob;
		mach_msg_type_number_t cnt = 0;

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
	/* __io_node picks /dev/ioregistry (IOREGIOCNODE) or hwregd; only
	 * the class field is wanted, so the rest are skipped (NULLs). */
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

kern_return_t
__io_pack_criteria(const struct io_criteria *c, uint8_t *blob,
    uint32_t *out_size)
{
	nvlist_t *crit;
	void *packed;
	size_t psz = 0;

	crit = nvlist_create_dictionary(0);
	if (crit == NULL)
		return (kIOReturnError);
	if (c->klass[0] != '\0')
		nvlist_add_string(crit, "class", c->klass);
	if (c->name[0] != '\0')
		nvlist_add_string(crit, "name", c->name);
	if (c->driver[0] != '\0')
		nvlist_add_string(crit, "driver", c->driver);

	packed = nvlist_pack(crit, &psz);
	nvlist_destroy(crit);
	if (packed == NULL || psz > sizeof(hwreg_blob_t)) {
		free(packed);
		return (kIOReturnError);
	}
	(void)memcpy(blob, packed, psz);
	free(packed);
	*out_size = (uint32_t)psz;
	return (KERN_SUCCESS);
}

/*
 * Pack a criteria struct into the FreeBSD KERNEL libnv wire format (#218).
 *
 * The kernel /dev/ioregistry IOREGIOC{WATCH,LOOKUP} handlers unpack the
 * criteria blob with the base system's sys/contrib/libnv nvlist_unpack(), whose
 * on-the-wire layout is NOT the same as this repo's libxpc nvlist (used by
 * __io_pack_criteria above and by the hwregd RPCs). Two concrete differences
 * make a libxpc-packed blob fail to unpack in the kernel:
 *
 *   - nvlist header: libxpc's struct nvlist_header has an extra uint8_t
 *     nvlh_type at offset 3 (a libxpc dictionary/array extension); the kernel's
 *     has none. So every following field is shifted one byte and the kernel's
 *     "nvlh_size == left - sizeof(header)" check fails (header sizes 20 vs 19).
 *   - nvpair header: the kernel's struct nvpair_header carries a trailing
 *     uint64_t nvph_nitems that libxpc's omits, shifting each pair by 8 bytes.
 *
 * Either mismatch makes the kernel nvlist_unpack() return NULL, so IOREGIOCWATCH
 * returns EINVAL and the watch is never registered (the #218 round-trip break).
 * The criteria are only ever a flat set of NV_TYPE_STRING pairs (name/class/
 * driver), so we serialize them directly in the kernel format here rather than
 * teach libxpc a second wire format.
 *
 * Kernel wire format (little-endian, __packed; mirrors sys/contrib/libnv):
 *   nvlist_header (19 bytes): u8 magic=0x6c, u8 version=0, u8 flags=0,
 *       u64 descriptors=0, u64 size=<bytes after this header>
 *   per pair (19 bytes + namesize + datasize): u8 type=3 (NV_TYPE_STRING),
 *       u16 namesize=strlen(key)+1, u64 datasize=strlen(val)+1, u64 nitems=0,
 *       key bytes (incl NUL), value bytes (incl NUL)
 */
#define	NV_HDR_MAGIC		0x6cu
#define	NVPAIR_TYPE_STRING	3u	/* sys/contrib/libnv NV_TYPE_STRING */

static void
le_put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void
le_put64(uint8_t *p, uint64_t v)
{
	int i;

	for (i = 0; i < 8; i++)
		p[i] = (uint8_t)((v >> (8 * i)) & 0xff);
}

/* Append one NV_TYPE_STRING pair to `blob` at *off (bounded by cap). Returns
 * KERN_SUCCESS or kIOReturnError on overflow. */
static kern_return_t
libnv_append_string(uint8_t *blob, size_t cap, size_t *off,
    const char *key, const char *val)
{
	size_t namesz = strlen(key) + 1;
	size_t datasz = strlen(val) + 1;
	size_t need = 1 + 2 + 8 + 8 + namesz + datasz;	/* nvpair_header + name + data */

	if (*off + need > cap)
		return (kIOReturnError);
	blob[*off + 0] = (uint8_t)NVPAIR_TYPE_STRING;
	le_put16(&blob[*off + 1], (uint16_t)namesz);
	le_put64(&blob[*off + 3], (uint64_t)datasz);
	le_put64(&blob[*off + 11], 0);			/* nvph_nitems == 0 for a string */
	*off += 19;
	(void)memcpy(&blob[*off], key, namesz);
	*off += namesz;
	(void)memcpy(&blob[*off], val, datasz);
	*off += datasz;
	return (KERN_SUCCESS);
}

kern_return_t
__io_pack_criteria_libnv(const struct io_criteria *c, uint8_t *blob,
    uint32_t *out_size)
{
	const size_t cap = sizeof(hwreg_blob_t);
	size_t off = 19;	/* leave room for the nvlist_header, filled last */
	kern_return_t kr;

	if (cap < 19)
		return (kIOReturnError);

	if (c->klass[0] != '\0' &&
	    (kr = libnv_append_string(blob, cap, &off, "class", c->klass))
	    != KERN_SUCCESS)
		return (kr);
	if (c->name[0] != '\0' &&
	    (kr = libnv_append_string(blob, cap, &off, "name", c->name))
	    != KERN_SUCCESS)
		return (kr);
	if (c->driver[0] != '\0' &&
	    (kr = libnv_append_string(blob, cap, &off, "driver", c->driver))
	    != KERN_SUCCESS)
		return (kr);

	/* nvlist_header: magic, version 0, flags 0, descriptors 0, size = body. */
	blob[0] = (uint8_t)NV_HDR_MAGIC;
	blob[1] = 0;			/* version */
	blob[2] = 0;			/* flags (must be 0: unpack(...,flags=0)) */
	le_put64(&blob[3], 0);		/* descriptors */
	le_put64(&blob[11], (uint64_t)(off - 19));	/* size after the header */

	*out_size = (uint32_t)off;
	return (KERN_SUCCESS);
}

/*
 * Translate `matching` to a hwreg_lookup criteria nvlist, pack it,
 * fire the RPC, return the malloc'd id array + count. Caller frees
 * `ids_out`. KERN_SUCCESS even when 0 matches — the result is
 * "no matches", not an error.
 */
#define IOKIT_MAX_MATCHES	128	/* hwreg.defs hwreg_id_array_t bound */

static kern_return_t
lookup_matches(CFDictionaryRef matching, uint64_t **ids_out,
    uint32_t *count_out)
{
	int fd = __io_ioregistry_fd();
	struct io_criteria c;
	hwreg_blob_t critblob;	/* packed criteria nvlist; same wire format
				 * for both /dev/ioregistry and hwregd */
	uint32_t psz = 0;
	uint64_t *ids;
	uint32_t cap = IOKIT_MAX_MATCHES;
	kern_return_t kr;

	__io_extract_criteria(matching, &c);
	/* Pack in the chosen backend's wire format: base-system libnv for the
	 * kernel /dev/ioregistry IOREGIOCLOOKUP handler, libxpc for the hwregd
	 * RPC fallback. The libxpc-vs-libnv mismatch is the #218 root cause (see
	 * __io_pack_criteria_libnv); the same fix applies to LOOKUP and WATCH. */
	kr = (fd >= 0)
	    ? __io_pack_criteria_libnv(&c, critblob, &psz)
	    : __io_pack_criteria(&c, critblob, &psz);
	if (kr != KERN_SUCCESS)
		return (kr);
	ids = calloc(cap, sizeof(*ids));
	if (ids == NULL)
		return (KERN_RESOURCE_SHORTAGE);

	if (fd >= 0) {
		struct ioreg_lookup lk;

		(void)memset(&lk, 0, sizeof(lk));
		/* crit_len 0 (no criteria survived extraction) matches every
		 * live node — same semantics as the kernel ABI documents. */
		lk.buf_criteria = (uint64_t)(uintptr_t)critblob;
		lk.crit_len = psz;
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

	/* Fallback: hwregd MIG. */
	{
		mach_port_t svc = __io_hwregd_port();
		mach_msg_type_number_t nids = cap;

		if (svc == MACH_PORT_NULL) {
			free(ids);
			return (kIOReturnNoDevice);
		}
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
