/*
 * test_list.c -- Tests for intrusive doubly-linked circular list (Round 5-0).
 */

#include "test/test.h"
#include "list.h"
#include "types.h"

struct item {
    int val;
    struct list_head link;
};

void
test_list(void) {
    kprintf("[list]\n");

    /* --- Empty list --- */

    LIST_HEAD(head);
    TEST_ASSERT(list_empty(&head), "new list is empty");
    TEST_ASSERT(head.next == &head, "empty list next points to self");
    TEST_ASSERT(head.prev == &head, "empty list prev points to self");

    /* --- INIT_LIST_HEAD (runtime init) --- */

    struct list_head dynamic;
    INIT_LIST_HEAD(&dynamic);
    TEST_ASSERT(list_empty(&dynamic), "INIT_LIST_HEAD makes empty list");

    /* --- list_add (front insertion) --- */

    struct item a = {.val = 1};
    struct item b = {.val = 2};
    struct item c = {.val = 3};

    list_add(&a.link, &head);
    TEST_ASSERT(!list_empty(&head), "list non-empty after add");
    TEST_ASSERT(list_is_singular(&head), "one entry is singular");
    TEST_ASSERT(list_first_entry(&head, struct item, link)->val == 1, "first entry is a");

    list_add(&b.link, &head);
    TEST_ASSERT(list_first_entry(&head, struct item, link)->val == 2, "list_add puts b at front");
    TEST_ASSERT(!list_is_singular(&head), "two entries not singular");

    list_add(&c.link, &head);
    TEST_ASSERT(list_first_entry(&head, struct item, link)->val == 3, "list_add puts c at front");

    /* Order should be: head -> c(3) -> b(2) -> a(1) -> head */

    /* --- list_for_each_entry --- */

    int expected[] = {3, 2, 1};
    int idx = 0;
    int order_ok = 1;
    struct item *pos;
    list_for_each_entry(pos, &head, link) {
        if (pos->val != expected[idx])
            order_ok = 0;
        idx++;
    }
    TEST_ASSERT(idx == 3, "for_each_entry visits 3 entries");
    TEST_ASSERT(order_ok, "list_add gives LIFO order (3,2,1)");

    /* --- list_del (middle removal) --- */

    list_del(&b.link);
    idx = 0;
    int after_del[] = {3, 1};
    order_ok = 1;
    list_for_each_entry(pos, &head, link) {
        if (pos->val != after_del[idx])
            order_ok = 0;
        idx++;
    }
    TEST_ASSERT(idx == 2, "after del, 2 entries remain");
    TEST_ASSERT(order_ok, "del removed middle entry (3,1)");

    /* --- list_del (head removal) --- */

    list_del(&c.link);
    TEST_ASSERT(list_first_entry(&head, struct item, link)->val == 1,
                "after removing c, first is a");
    TEST_ASSERT(list_is_singular(&head), "one entry left is singular");

    /* --- list_del (last entry) --- */

    list_del(&a.link);
    TEST_ASSERT(list_empty(&head), "removing last entry makes list empty");

    /* --- list_add_tail (FIFO order) --- */

    struct item d = {.val = 10};
    struct item e = {.val = 20};
    struct item f = {.val = 30};

    LIST_HEAD(fifo);
    list_add_tail(&d.link, &fifo);
    list_add_tail(&e.link, &fifo);
    list_add_tail(&f.link, &fifo);

    int fifo_expected[] = {10, 20, 30};
    idx = 0;
    order_ok = 1;
    list_for_each_entry(pos, &fifo, link) {
        if (pos->val != fifo_expected[idx])
            order_ok = 0;
        idx++;
    }
    TEST_ASSERT(idx == 3, "tail-add visits 3 entries");
    TEST_ASSERT(order_ok, "list_add_tail gives FIFO order (10,20,30)");

    /* --- list_for_each_entry_safe (delete while iterating) --- */

    struct item *tmp;
    list_for_each_entry_safe(pos, tmp, &fifo, link) {
        if (pos->val == 20)
            list_del(&pos->link);
    }
    idx = 0;
    int safe_expected[] = {10, 30};
    order_ok = 1;
    list_for_each_entry(pos, &fifo, link) {
        if (pos->val != safe_expected[idx])
            order_ok = 0;
        idx++;
    }
    TEST_ASSERT(idx == 2, "safe iteration + del leaves 2 entries");
    TEST_ASSERT(order_ok, "safe del removed middle (10,30)");

    /* --- container_of correctness --- */

    struct item g = {.val = 42};
    list_add(&g.link, &fifo);
    struct list_head *raw = fifo.next;
    struct item *recovered = container_of(raw, struct item, link);
    TEST_ASSERT(recovered->val == 42, "container_of recovers correct struct");

    /* --- Multiple list membership --- */

    struct multi {
        int id;
        struct list_head list_a;
        struct list_head list_b;
    };

    LIST_HEAD(group_a);
    LIST_HEAD(group_b);

    struct multi m1 = {.id = 1};
    struct multi m2 = {.id = 2};

    INIT_LIST_HEAD(&m1.list_a);
    INIT_LIST_HEAD(&m1.list_b);
    INIT_LIST_HEAD(&m2.list_a);
    INIT_LIST_HEAD(&m2.list_b);

    list_add_tail(&m1.list_a, &group_a);
    list_add_tail(&m2.list_a, &group_a);
    list_add_tail(&m1.list_b, &group_b);
    list_add_tail(&m2.list_b, &group_b);

    /* Both lists have 2 entries. */
    idx = 0;
    struct multi *mp;
    list_for_each_entry(mp, &group_a, list_a) idx++;
    TEST_ASSERT(idx == 2, "group_a has 2 entries");

    idx = 0;
    list_for_each_entry(mp, &group_b, list_b) idx++;
    TEST_ASSERT(idx == 2, "group_b has 2 entries");

    /* Remove m1 from group_a only; it should remain in group_b. */
    list_del(&m1.list_a);
    idx = 0;
    list_for_each_entry(mp, &group_a, list_a) idx++;
    TEST_ASSERT(idx == 1, "group_a has 1 after removing m1");

    idx = 0;
    list_for_each_entry(mp, &group_b, list_b) idx++;
    TEST_ASSERT(idx == 2, "group_b still has 2 (independent lists)");
}
