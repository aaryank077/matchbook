#include "pool.h"

#include <stdlib.h>

int pool_init(pool *p, size_t obj_size, size_t capacity) {
    if (obj_size < sizeof(void *)) {
        obj_size = sizeof(void *);
    }
    /* Round up so every slot is pointer-aligned. */
    obj_size = (obj_size + _Alignof(max_align_t) - 1) & ~(_Alignof(max_align_t) - 1);

    p->slab = malloc(obj_size * capacity);
    if (p->slab == NULL && capacity > 0) {
        return -1;
    }
    p->obj_size = obj_size;
    p->capacity = capacity;
    p->in_use = 0;
    p->free_head = NULL;

    /* Thread the free list back to front so allocation starts at slot 0. */
    for (size_t i = capacity; i-- > 0;) {
        void *slot = p->slab + i * obj_size;
        *(void **) slot = p->free_head;
        p->free_head = slot;
    }
    return 0;
}

void pool_destroy(pool *p) {
    free(p->slab);
    p->slab = NULL;
    p->free_head = NULL;
    p->capacity = p->in_use = 0;
}

void *pool_alloc(pool *p) {
    void *slot = p->free_head;
    if (slot != NULL) {
        p->free_head = *(void **) slot;
        p->in_use++;
    }
    return slot;
}

void pool_free(pool *p, void *obj) {
    *(void **) obj = p->free_head;
    p->free_head = obj;
    p->in_use--;
}
