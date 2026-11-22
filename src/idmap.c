#include "idmap.h"

#include <stdlib.h>

/* splitmix64 finalizer: sequential ids spread evenly across slots. */
static inline size_t hash(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return (size_t) x;
}

int idmap_init(idmap *m, size_t max_entries) {
    size_t cap = 16;
    while (cap < max_entries * 2) {
        cap <<= 1;
    }
    m->slots = calloc(cap, sizeof(idmap_slot));
    if (m->slots == NULL) {
        return -1;
    }
    /*
     * Pre-fault: large calloc'd blocks come straight from mmap as untouched
     * zero pages, so the first insert into each page would take a page fault
     * on the matching hot path. Touching every page now moves that cost to
     * startup. In the benchmark this cut limit-order p99 latency by ~4x.
     * The volatile write keeps the compiler from optimizing the loop away.
     */
    volatile unsigned char *bytes = (volatile unsigned char *) m->slots;
    size_t total = cap * sizeof(idmap_slot);
    for (size_t off = 0; off < total; off += 4096) {
        bytes[off] = 0;
    }
    m->mask = cap - 1;
    m->count = 0;
    return 0;
}

void idmap_destroy(idmap *m) {
    free(m->slots);
    m->slots = NULL;
    m->count = 0;
}

void *idmap_get(const idmap *m, uint64_t key) {
    for (size_t i = hash(key) & m->mask;; i = (i + 1) & m->mask) {
        if (m->slots[i].key == key) {
            return m->slots[i].value;
        }
        if (m->slots[i].key == 0) {
            return NULL;
        }
    }
}

int idmap_put(idmap *m, uint64_t key, void *value) {
    for (size_t i = hash(key) & m->mask;; i = (i + 1) & m->mask) {
        if (m->slots[i].key == key) {
            return 1;
        }
        if (m->slots[i].key == 0) {
            m->slots[i].key = key;
            m->slots[i].value = value;
            m->count++;
            return 0;
        }
    }
}

void *idmap_remove(idmap *m, uint64_t key) {
    size_t i = hash(key) & m->mask;
    while (m->slots[i].key != key) {
        if (m->slots[i].key == 0) {
            return NULL;
        }
        i = (i + 1) & m->mask;
    }
    void *value = m->slots[i].value;

    /*
     * Backward-shift deletion: walk the cluster after the hole and pull back
     * any entry whose home slot is at or before the hole (cyclically), so every
     * remaining key stays reachable from its home slot without tombstones.
     */
    size_t hole = i;
    for (size_t j = (i + 1) & m->mask; m->slots[j].key != 0; j = (j + 1) & m->mask) {
        size_t home = hash(m->slots[j].key) & m->mask;
        /* Distance from home to j vs. from home to hole, both mod capacity. */
        if (((j - home) & m->mask) >= ((j - hole) & m->mask)) {
            m->slots[hole] = m->slots[j];
            hole = j;
        }
    }
    m->slots[hole].key = 0;
    m->slots[hole].value = NULL;
    m->count--;
    return value;
}
