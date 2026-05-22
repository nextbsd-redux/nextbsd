/*
 * config_wire.h — the byte encodings configd's batch routines use to
 * carry a list of keys, or a map of key/value pairs, inside a single
 * inline config.defs payload (configd iter 7).
 *
 * Apple's configd serializes a CFArray / CFDictionary into each batch
 * argument; this CF-free port packs them as plain length-prefixed
 * records instead. Two formats, both little-endian:
 *
 *   key list   — a run of   [uint32 klen][klen key bytes]
 *   key/value  — a run of   [uint32 klen][key][uint32 vlen][val bytes]
 *
 * The key-list format is the one notifychanges and configlist already
 * emit; gathering it here lets notifyset / configget_m / configset_m
 * (and their test clients) share one decoder. Each *_put appends a
 * record and fails cleanly if the buffer is full; each *_next steps a
 * cursor and stops at the end or at the first malformed record, so a
 * truncated or hostile payload can never walk off the buffer.
 */

#ifndef _CONFIG_WIRE_H
#define _CONFIG_WIRE_H

#include <sys/types.h>
#include <stdint.h>

/*
 * wire_keylist_put — append one [uint32 len][bytes] key record to buf
 * at offset *off (capacity cap), advancing *off. Returns 0, or -1 if
 * the record would not fit.
 */
int wire_keylist_put(uint8_t *buf, size_t cap, size_t *off,
    const void *key, size_t klen);

/*
 * wire_keylist_next — step a cursor through a packed key list. *cur
 * starts at the buffer; `end` is one past its last byte. Returns 1
 * with *key / *klen set to the next record (and advances *cur), or 0
 * at the end or on a truncated record.
 */
int wire_keylist_next(const uint8_t **cur, const uint8_t *end,
    const void **key, size_t *klen);

/*
 * wire_kvmap_put — append one [uint32 klen][key][uint32 vlen][val]
 * record. Returns 0, or -1 if it would not fit.
 */
int wire_kvmap_put(uint8_t *buf, size_t cap, size_t *off,
    const void *key, size_t klen, const void *val, size_t vlen);

/*
 * wire_kvmap_next — step a cursor through a packed key/value map.
 * Returns 1 with *key / *klen / *val / *vlen set to the next record
 * (and advances *cur), or 0 at the end or on a truncated record.
 */
int wire_kvmap_next(const uint8_t **cur, const uint8_t *end,
    const void **key, size_t *klen, const void **val, size_t *vlen);

#endif /* !_CONFIG_WIRE_H */
