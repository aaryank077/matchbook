/*
 * Fixed-size object pool. All memory is allocated once up front; alloc and
 * free are O(1) pops/pushes on an intrusive free list threaded through the
 * unused slots, like a kernel slab cache. Nothing on the matching hot path
 * calls malloc.
 */
#ifndef MB_POOL_H
#define MB_POOL_H

#include <stddef.h>

typedef struct pool {
    unsigned char *slab;
    size_t obj_size;
    size_t capacity;
    size_t in_use;
    void *free_head;
} pool;

/* Returns 0 on success, -1 if memory could not be allocated. */
int pool_init(pool *p, size_t obj_size, size_t capacity);
void pool_destroy(pool *p);

/* Returns NULL when the pool is exhausted. */
void *pool_alloc(pool *p);
void pool_free(pool *p, void *obj);

#endif
