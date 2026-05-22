/*
 * config_store.c — configd's SCDynamicStore key/value store (iter 2).
 *
 * iter 2 keeps the store deliberately simple: a dynamically grown
 * array of {key, value} byte-blob pairs with linear lookup. The live
 * store holds at most a few hundred keys, and every access is on the
 * single mach_msg receive thread, so a hash table / locking would be
 * premature. iter 4 (change notifications + pattern keys) is the
 * point to revisit the data structure.
 */

#include "config_store.h"

#include <stdlib.h>
#include <string.h>

struct entry {
	void	*key;
	size_t	klen;
	void	*val;
	size_t	vlen;
};

static struct entry	*entries;
static size_t		n_entries;
static size_t		cap;

static struct entry *
store_find(const void *key, size_t klen)
{
	size_t i;

	for (i = 0; i < n_entries; i++) {
		if (entries[i].klen == klen &&
		    memcmp(entries[i].key, key, klen) == 0)
			return &entries[i];
	}
	return NULL;
}

int
store_set(const void *key, size_t klen, const void *val, size_t vlen)
{
	struct entry	*e;
	void		*vcopy;

	/* malloc(0) may return NULL; ask for at least one byte. */
	vcopy = malloc(vlen != 0 ? vlen : 1);
	if (vcopy == NULL)
		return -1;
	if (vlen != 0)
		memcpy(vcopy, val, vlen);

	/* Existing key — swap the value in place. */
	e = store_find(key, klen);
	if (e != NULL) {
		free(e->val);
		e->val = vcopy;
		e->vlen = vlen;
		return 0;
	}

	/* New key — grow the array if it is full. */
	if (n_entries == cap) {
		size_t		ncap = (cap != 0) ? cap * 2 : 16;
		struct entry	*ne;

		ne = realloc(entries, ncap * sizeof(*ne));
		if (ne == NULL) {
			free(vcopy);
			return -1;
		}
		entries = ne;
		cap = ncap;
	}

	e = &entries[n_entries];
	e->key = malloc(klen != 0 ? klen : 1);
	if (e->key == NULL) {
		free(vcopy);
		return -1;
	}
	if (klen != 0)
		memcpy(e->key, key, klen);
	e->klen = klen;
	e->val = vcopy;
	e->vlen = vlen;
	n_entries++;
	return 0;
}

int
store_get(const void *key, size_t klen, const void **val, size_t *vlen)
{
	struct entry *e;

	e = store_find(key, klen);
	if (e == NULL)
		return -1;
	*val = e->val;
	*vlen = e->vlen;
	return 0;
}

int
store_remove(const void *key, size_t klen)
{
	struct entry	*e;
	size_t		idx;

	e = store_find(key, klen);
	if (e == NULL)
		return -1;

	idx = (size_t)(e - entries);
	free(e->key);
	free(e->val);
	/* Compact: move the last entry into the freed slot. */
	entries[idx] = entries[--n_entries];
	return 0;
}

size_t
store_count(void)
{
	return n_entries;
}

int
store_key_at(size_t idx, const void **key, size_t *klen)
{
	if (idx >= n_entries)
		return -1;
	*key = entries[idx].key;
	*klen = entries[idx].klen;
	return 0;
}
