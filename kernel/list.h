/*
 * list.h - intrusive circular doubly-linked list (the Linux list_head idea).
 *
 * "Intrusive" means the link lives *inside* the object (a TCB holds a
 * k_list_t), so putting a task on a list never allocates memory. Every list
 * has a sentinel head node, so the empty list points to itself and insert /
 * remove never need special cases.
 */
#ifndef KESTREL_LIST_H
#define KESTREL_LIST_H

#include <stddef.h>

typedef struct k_list {
    struct k_list *next, *prev;
} k_list_t;

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define list_for_each(it, head) \
    for ((it) = (head)->next; (it) != (head); (it) = (it)->next)

static inline void list_init(k_list_t *head) { head->next = head->prev = head; }

static inline int list_empty(const k_list_t *head) { return head->next == head; }

/* A node is "linked" while it sits on some list; list_remove clears it. */
static inline int list_linked(const k_list_t *node) { return node->next != NULL; }

static inline void list_insert_before(k_list_t *pos, k_list_t *node)
{
    node->prev = pos->prev;
    node->next = pos;
    pos->prev->next = node;
    pos->prev = node;
}

static inline void list_push_back(k_list_t *head, k_list_t *node)
{
    list_insert_before(head, node);
}

static inline void list_push_front(k_list_t *head, k_list_t *node)
{
    list_insert_before(head->next, node);
}

static inline void list_remove(k_list_t *node)
{
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->next = node->prev = NULL;
}

#endif
