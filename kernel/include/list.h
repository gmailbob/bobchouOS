/*
 * list.h — Intrusive doubly-linked circular list.
 *
 * See Lecture 5-0 for design rationale (why intrusive, offsetof/container_of,
 * circular + sentinel design, invariants).
 *
 * == Public API ==
 *
 *   Initialization:
 *     LIST_HEAD(name)                   declare + static init
 *     INIT_LIST_HEAD(head)              runtime init
 *
 *   Insertion:
 *     list_add(new, head)               insert after head (LIFO)
 *     list_add_tail(new, head)          insert before head (FIFO)
 *
 *   Removal:
 *     list_del(entry)                   unlink from list, O(1)
 *
 *   Queries:
 *     list_empty(head)                  is list empty?
 *     list_is_singular(head)            exactly one entry?
 *     list_first_entry(head, type, member)  first enclosing struct
 *
 *   Iteration:
 *     list_for_each(pos, head)                        raw node iteration
 *     list_for_each_safe(pos, tmp, head)              safe node iteration
 *     list_for_each_entry(pos, head, member)          struct iteration
 *     list_for_each_entry_safe(pos, tmp, head, member) safe struct iteration
 *
 *   Helpers:
 *     container_of(ptr, type, member)   node pointer -> enclosing struct
 *     list_entry(ptr, type, member)     alias for container_of
 *
 * == Usage ==
 *
 *   // 1. Embed list_head in your struct:
 *   struct task {
 *       int pid;
 *       struct list_head run_list;   // link for run queue
 *       struct list_head all_list;   // link for global task list
 *   };
 *
 *   // 2. Declare a list head (the sentinel / anchor):
 *   LIST_HEAD(run_queue);            // static init
 *   // or at runtime:
 *   struct list_head run_queue;
 *   INIT_LIST_HEAD(&run_queue);
 *
 *   // 3. Add entries:
 *   struct task t1 = { .pid = 1 };
 *   list_add_tail(&t1.run_list, &run_queue);  // enqueue (FIFO)
 *   list_add(&t1.run_list, &run_queue);       // push front (LIFO)
 *
 *   // 4. Iterate (get the enclosing struct, not the raw node):
 *   struct task *pos;
 *   list_for_each_entry(pos, &run_queue, run_list) {
 *       kprintf("pid = %d\n", pos->pid);
 *   }
 *
 *   // 5. Remove (O(1), only needs the node pointer):
 *   list_del(&t1.run_list);
 *
 *   // 6. Delete while iterating (safe version):
 *   struct task *tmp;
 *   list_for_each_entry_safe(pos, tmp, &run_queue, run_list) {
 *       if (pos->pid == 42)
 *           list_del(&pos->run_list);
 *   }
 *
 *   // 7. Check state:
 *   list_empty(&run_queue);          // 1 if no entries
 *   list_is_singular(&run_queue);    // 1 if exactly one entry
 *   list_first_entry(&run_queue, struct task, run_list);  // first entry
 */

#ifndef LIST_H
#define LIST_H

#include "types.h"

/*
 * offsetof — byte offset of a member within a struct.
 */
#define offsetof(type, member) ((uint64) & ((type *)0)->member)

/*
 * container_of — given a pointer to a member, recover the enclosing struct.
 *
 * Example:
 *   struct task { int pid; struct list_head run_list; };
 *   struct list_head *ptr = ...;
 *   struct task *t = container_of(ptr, struct task, run_list);
 */
#define container_of(ptr, type, member) ((type *)((char *)(ptr)-offsetof(type, member)))

/*
 * The list node. Embed this in your struct. Also used as the list head
 * (sentinel node that is not itself inside a container struct).
 */
struct list_head {
    struct list_head *prev;
    struct list_head *next;
};

/*
 * LIST_HEAD — declare and initialize a list head variable (points to itself).
 */
#define LIST_HEAD(name) struct list_head name = {&(name), &(name)}

/*
 * INIT_LIST_HEAD — runtime initialization for a list head.
 */
static inline void
INIT_LIST_HEAD(struct list_head *head) {
    head->next = head;
    head->prev = head;
}

/*
 * __list_add — internal: insert a new entry between two consecutive entries.
 */
static inline void
__list_add(struct list_head *new, struct list_head *prev, struct list_head *next) {
    prev->next = new;
    new->next = next;
    next->prev = new;
    new->prev = prev;
}

/*
 * list_add — insert new entry immediately after head (front of list).
 * Use for stack-like (LIFO) behavior.
 */
static inline void
list_add(struct list_head *new, struct list_head *head) {
    __list_add(new, head, head->next);
}

/*
 * list_add_tail — insert new entry immediately before head (end of list).
 * Use for queue-like (FIFO) behavior.
 */
static inline void
list_add_tail(struct list_head *new, struct list_head *head) {
    __list_add(new, head->prev, head);
}

/*
 * __list_del — internal: remove entry by connecting prev and next to each other.
 */
static inline void
__list_del(struct list_head *prev, struct list_head *next) {
    prev->next = next;
    next->prev = prev;
}

/*
 * list_del — remove an entry from whatever list it's in.
 * The entry's prev/next are set to NULL after removal so that
 * a double-delete is easier to catch.
 */
static inline void
list_del(struct list_head *entry) {
    __list_del(entry->prev, entry->next);
    entry->prev = NULL;
    entry->next = NULL;
}

/*
 * list_empty — return 1 if the list has no entries (head points to itself).
 */
static inline int
list_empty(struct list_head *head) {
    /* one direction suffices if the list is well-formed (symmetric invariant) */
    return head->next == head;
}

/*
 * list_is_singular — return 1 if the list has exactly one entry.
 */
static inline int
list_is_singular(struct list_head *head) {
    /* non-empty and only one entry: next and prev both point to the same node */
    return !list_empty(head) && head->next == head->prev;
}

/*
 * list_entry — get the struct for this list node.
 * Wrapper around container_of for readability.
 *
 * Usage: struct task *t = list_entry(ptr, struct task, run_list);
 */
#define list_entry(ptr, type, member) container_of(ptr, type, member)

/*
 * list_first_entry — get the first element from a non-empty list.
 */
#define list_first_entry(head, type, member) list_entry((head)->next, type, member)

/*
 * list_for_each — iterate over list nodes (struct list_head pointers).
 * Use when you don't need the enclosing struct, or will call container_of yourself.
 *
 * @pos:  struct list_head * used as loop cursor
 * @head: the list head (sentinel)
 */
#define list_for_each(pos, head) for (pos = (head)->next; pos != (head); pos = pos->next)

/*
 * list_for_each_safe — iterate with safe deletion.
 * Saves the next pointer before the loop body, so you can list_del(pos)
 * inside the loop.
 *
 * @pos:  struct list_head * loop cursor
 * @tmp:  struct list_head * temporary for safe iteration
 * @head: the list head (sentinel)
 */
#define list_for_each_safe(pos, tmp, head)                                                         \
    for (pos = (head)->next, tmp = pos->next; pos != (head); pos = tmp, tmp = pos->next)

/*
 * list_for_each_entry — iterate over list entries (enclosing structs).
 *
 * @pos:    type * used as loop cursor
 * @head:   the list head (sentinel)
 * @member: name of the list_head field within the struct
 */
#define list_for_each_entry(pos, head, member)                                                     \
    for (pos = list_entry((head)->next, typeof(*pos), member); &pos->member != (head);             \
         pos = list_entry(pos->member.next, typeof(*pos), member))

/*
 * list_for_each_entry_safe — iterate with safe deletion (entry version).
 *
 * @pos:    type * loop cursor
 * @tmp:    type * temporary for safe iteration
 * @head:   the list head (sentinel)
 * @member: name of the list_head field within the struct
 */
#define list_for_each_entry_safe(pos, tmp, head, member)                                           \
    for (pos = list_entry((head)->next, typeof(*pos), member),                                     \
        tmp = list_entry(pos->member.next, typeof(*pos), member);                                  \
         &pos->member != (head);                                                                   \
         pos = tmp, tmp = list_entry(tmp->member.next, typeof(*tmp), member))

#endif /* LIST_H */
