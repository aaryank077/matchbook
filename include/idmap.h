/*
 * Open-addressing hash map from order id (uint64_t, nonzero) to a pointer.
 * Linear probing with backward-shift deletion, so there are no tombstones and
 * lookups stay fast no matter how many cancels have happened. Capacity is
 * fixed at creation (a power of two at least twice the max number of entries).
 */
#ifndef MB_IDMAP_H
#define MB_IDMAP_H

#include <stddef.h>
#include <stdint.h>

typedef struct idmap_slot {
    uint64_t key; /* 0 means empty */
    void *value;
} idmap_slot;

typedef struct idmap {
    idmap_slot *slots;
    size_t mask;
    size_t count;
} idmap;

int idmap_init(idmap *m, size_t max_entries);
void idmap_destroy(idmap *m);

void *idmap_get(const idmap *m, uint64_t key);
/* Returns 0 on insert, 1 if the key already existed (value left unchanged). */
int idmap_put(idmap *m, uint64_t key, void *value);
/* Returns the removed value, or NULL if absent. */
void *idmap_remove(idmap *m, uint64_t key);

#endif
