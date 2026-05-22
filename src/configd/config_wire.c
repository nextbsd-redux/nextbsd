/*
 * config_wire.c — key-list and key/value-map byte encodings for
 * configd's batch routines (configd iter 7). See config_wire.h.
 */

#include "config_wire.h"

#include <string.h>

int
wire_keylist_put(uint8_t *buf, size_t cap, size_t *off,
    const void *key, size_t klen)
{
	uint32_t l32 = (uint32_t)klen;

	if (*off + sizeof(l32) + klen > cap)
		return -1;
	memcpy(buf + *off, &l32, sizeof(l32));
	*off += sizeof(l32);
	memcpy(buf + *off, key, klen);
	*off += klen;
	return 0;
}

int
wire_keylist_next(const uint8_t **cur, const uint8_t *end,
    const void **key, size_t *klen)
{
	const uint8_t	*p = *cur;
	uint32_t	l32;

	if ((size_t)(end - p) < sizeof(l32))
		return 0;
	memcpy(&l32, p, sizeof(l32));
	p += sizeof(l32);
	if ((size_t)(end - p) < l32)
		return 0;		/* truncated record */
	*key = p;
	*klen = l32;
	*cur = p + l32;
	return 1;
}

int
wire_kvmap_put(uint8_t *buf, size_t cap, size_t *off,
    const void *key, size_t klen, const void *val, size_t vlen)
{
	uint32_t kl = (uint32_t)klen;
	uint32_t vl = (uint32_t)vlen;

	if (*off + sizeof(kl) + klen + sizeof(vl) + vlen > cap)
		return -1;
	memcpy(buf + *off, &kl, sizeof(kl));
	*off += sizeof(kl);
	memcpy(buf + *off, key, klen);
	*off += klen;
	memcpy(buf + *off, &vl, sizeof(vl));
	*off += sizeof(vl);
	memcpy(buf + *off, val, vlen);
	*off += vlen;
	return 0;
}

int
wire_kvmap_next(const uint8_t **cur, const uint8_t *end,
    const void **key, size_t *klen, const void **val, size_t *vlen)
{
	const uint8_t	*p = *cur;
	uint32_t	kl;
	uint32_t	vl;

	if ((size_t)(end - p) < sizeof(kl))
		return 0;
	memcpy(&kl, p, sizeof(kl));
	p += sizeof(kl);
	if ((size_t)(end - p) < kl)
		return 0;		/* truncated key */
	*key = p;
	*klen = kl;
	p += kl;

	if ((size_t)(end - p) < sizeof(vl))
		return 0;
	memcpy(&vl, p, sizeof(vl));
	p += sizeof(vl);
	if ((size_t)(end - p) < vl)
		return 0;		/* truncated value */
	*val = p;
	*vlen = vl;
	*cur = p + vl;
	return 1;
}
