/*
 * config_store.h — configd's SCDynamicStore key/value store (iter 2).
 *
 * A flat key->value table. Keys are the UTF-8 bytes of an
 * SCDynamicStore key (what CFStringCreateExternalRepresentation
 * produces, and what _SCSerializeString sends on the wire); values
 * are opaque serialized-property-list blobs that configd stores and
 * returns verbatim — it never interprets them. Keys and values are
 * both copied in, so callers keep ownership of what they pass.
 *
 * iter 2 runs entirely on configd's single mach_msg receive thread,
 * so the store needs no locking. iter 4's change notifications will
 * add the per-key watcher / pattern bookkeeping Apple keeps in a
 * sub-dictionary alongside each value.
 */

#ifndef _CONFIG_STORE_H
#define _CONFIG_STORE_H

#include <sys/types.h>

/*
 * store_set — insert key, or replace its value if already present.
 * The key and value bytes are both copied. Returns 0 on success,
 * -1 on an allocation failure.
 */
int store_set(const void *key, size_t klen, const void *val, size_t vlen);

/*
 * store_get — look key up. On a hit, *val points at the stored value
 * (borrowed — valid only until the next store mutation) and *vlen is
 * its length; returns 0. Returns -1 if the key is absent.
 */
int store_get(const void *key, size_t klen, const void **val, size_t *vlen);

/*
 * store_remove — drop key. Returns 0 if it was removed, -1 if absent.
 */
int store_remove(const void *key, size_t klen);

#endif /* _CONFIG_STORE_H */
