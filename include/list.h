/*
 * Intrusive circular doubly linked list, in the style of the Pintos kernel's
 * lib/kernel/list.h. The links live inside the element itself, so insert and
 * remove never allocate, and an element can be unlinked in O(1) given only a
 * pointer to it (which is what makes order cancellation O(1)).
 */
#ifndef MB_LIST_H
#define MB_LIST_H

#include <stddef.h>
#include <stdbool.h>

typedef struct list_elem {
    struct list_elem *prev;
    struct list_elem *next;
} list_elem;

/* A list is a sentinel element: head.next is the front, head.prev the back. */
typedef struct list {
    list_elem head;
} list;

/* Converts a pointer to an embedded list_elem back to its containing struct. */
#define list_entry(ELEM, STRUCT, MEMBER) \
    ((STRUCT *) ((char *) (ELEM) - offsetof(STRUCT, MEMBER)))

static inline void list_init(list *l) {
    l->head.prev = &l->head;
    l->head.next = &l->head;
}

static inline bool list_empty(const list *l) {
    return l->head.next == &l->head;
}

static inline list_elem *list_front(list *l) {
    return l->head.next;
}

static inline void list_push_back(list *l, list_elem *e) {
    e->prev = l->head.prev;
    e->next = &l->head;
    l->head.prev->next = e;
    l->head.prev = e;
}

static inline void list_remove(list_elem *e) {
    e->prev->next = e->next;
    e->next->prev = e->prev;
    e->prev = e->next = NULL;
}

#endif
