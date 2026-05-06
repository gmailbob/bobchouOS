/*
 * hashtable.h — Intrusive hash table with separate chaining.
 *
 * See Lecture 5-0 for design rationale (why hlist vs list_head for buckets,
 * the pprev trick, multiplicative hashing, collision handling).
 *
 * == Public API ==
 *
 *   Table setup:
 *     DEFINE_HASHTABLE(name, bits)       declare + zero-init (2^bits buckets)
 *     HT_SIZE(bits)                      number of buckets
 *     hash_init(ht, bits)                runtime zero-init
 *
 *   Insertion / removal:
 *     hash_add(ht, node, bits, hash)     insert into computed bucket
 *     hash_del(node)                     remove from table, O(1)
 *
 *   Queries:
 *     hlist_unhashed(node)               is node in a table?
 *
 *   Hash function:
 *     hash_int(val)                      multiplicative hash for integers
 *
 *   Iteration (within one bucket):
 *     hlist_for_each(pos, head)                  raw node iteration
 *     hlist_for_each_safe(pos, tmp, head)        safe node iteration
 *     hlist_for_each_entry(pos, head, member)    struct iteration
 *
 *   Helpers:
 *     hlist_entry(ptr, type, member)     node pointer -> enclosing struct
 *
 * == Usage ==
 *
 *   // 1. Embed hlist_node in your struct:
 *   struct proc {
 *       int pid;
 *       struct hlist_node pid_link;  // hash table membership
 *   };
 *
 *   // 2. Declare a hash table (fixed-size array of bucket heads):
 *   #define PID_HASH_BITS 6              // 64 buckets
 *   DEFINE_HASHTABLE(pid_table, PID_HASH_BITS);  // static init
 *   // or at runtime:
 *   struct hlist_head pid_table[HT_SIZE(PID_HASH_BITS)];
 *   hash_init(pid_table, PID_HASH_BITS);
 *
 *   // 3. Insert (caller computes the hash):
 *   struct proc p = { .pid = 42 };
 *   hash_add(pid_table, &p.pid_link, PID_HASH_BITS, hash_int(p.pid));
 *
 *   // 4. Lookup by key (iterate the bucket, compare keys):
 *   uint64 bucket = hash_int(42) & (HT_SIZE(PID_HASH_BITS) - 1);
 *   struct hlist_node *pos;
 *   hlist_for_each(pos, &pid_table[bucket]) {
 *       struct proc *found = hlist_entry(pos, struct proc, pid_link);
 *       if (found->pid == 42)
 *           break;  // got it
 *   }
 *
 *   // 5. Remove (O(1), only needs the node pointer):
 *   hash_del(&p.pid_link);
 *
 *   // 6. Check if a node is currently in a table:
 *   hlist_unhashed(&p.pid_link);    // 1 if not in any table
 */

#ifndef HASHTABLE_H
#define HASHTABLE_H

#include "types.h"
#include "list.h" /* for container_of, offsetof */

/*
 * hlist_head — bucket head. Points to the first node in the chain, or NULL.
 */
struct hlist_head {
    struct hlist_node *first;
};

/*
 * hlist_node — embedded in the entry struct. Participates in one bucket chain.
 *
 * pprev is a pointer to the pointer that points to us:
 *   - if we're first in bucket: pprev = &head->first
 *   - if we're after another node: pprev = &prev_node->next
 * This lets us unlink without scanning from the head.
 */
struct hlist_node {
    struct hlist_node *next;
    struct hlist_node **pprev;
};

/*
 * DEFINE_HASHTABLE — declare and zero-initialize a hash table array.
 *
 * @name: variable name
 * @bits: log2 of number of buckets (e.g., 4 → 16 buckets)
 */
#define HT_SIZE(bits) (1 << (bits))

#define DEFINE_HASHTABLE(name, bits)                                                               \
    struct hlist_head name[HT_SIZE(bits)] = {[0 ... HT_SIZE(bits) - 1] = {.first = NULL}}

/*
 * hash_init — runtime initialization (zero all bucket heads).
 *
 * @ht:   the hash table array
 * @bits: log2 of number of buckets
 */
static inline void
hash_init(struct hlist_head *ht, int bits) {
    /* TODO: set every bucket's first pointer to NULL. */
}

/*
 * HLIST_HEAD_INIT — static initializer for a single bucket.
 */
#define HLIST_HEAD_INIT                                                                            \
    { .first = NULL }

/*
 * hlist_unhashed — return 1 if node is not in any hash chain.
 * A node is unhashed if pprev is NULL.
 */
static inline int
hlist_unhashed(struct hlist_node *node) {
    /* TODO */
    return 0;
}

/*
 * hlist_add_head — insert node at the front of a bucket chain.
 */
static inline void
hlist_add_head(struct hlist_node *node, struct hlist_head *head) {
    /*
     * TODO: insert node before the current first node.
     * Steps:
     *   1. node->next = current first
     *   2. if current first exists, update its pprev
     *   3. head->first = node
     *   4. node->pprev = &head->first
     */
}

/*
 * hlist_del — remove a node from its bucket chain.
 * Sets pprev to NULL after removal (marks as unhashed).
 */
static inline void
hlist_del(struct hlist_node *node) {
    /*
     * TODO: unlink node using pprev and next pointers.
     * Steps:
     *   1. *node->pprev = node->next  (predecessor now points to our successor)
     *   2. if node->next exists, update its pprev
     *   3. node->next = NULL, node->pprev = NULL
     */
}

/*
 * hash_add — insert entry into hash table at bucket determined by key hash.
 *
 * @ht:   the hash table array
 * @node: the hlist_node embedded in the entry
 * @bits: log2 of number of buckets
 * @hash: pre-computed hash value of the key
 */
static inline void
hash_add(struct hlist_head *ht, struct hlist_node *node, int bits, uint64 hash) {
    /* TODO: compute bucket index from hash and bits, then hlist_add_head. */
}

/*
 * hash_del — remove an entry from the hash table.
 */
static inline void
hash_del(struct hlist_node *node) {
    hlist_del(node);
}

/*
 * hash_int — simple integer hash (multiplicative hashing).
 * Good enough for PID lookup. Returns a full 64-bit hash;
 * caller masks to the needed bits.
 */
static inline uint64
hash_int(uint64 val) {
    /*
     * TODO: implement multiplicative hash.
     * Multiply val by a large odd constant (golden ratio hash works well):
     *   val * 0x61C8864680B583EBull
     * Return the result (caller will mask to the needed bits via hash_add).
     */
    return 0;
}

/*
 * hlist_entry — get the struct containing this hlist_node.
 */
#define hlist_entry(ptr, type, member) container_of(ptr, type, member)

/*
 * hlist_for_each — iterate over nodes in a bucket.
 *
 * @pos:  struct hlist_node * loop cursor
 * @head: struct hlist_head * bucket head
 */
#define hlist_for_each(pos, head) for (pos = (head)->first; pos != NULL; pos = pos->next)

/*
 * hlist_for_each_safe — iterate with safe deletion.
 *
 * @pos:  struct hlist_node * loop cursor
 * @tmp:  struct hlist_node * temporary
 * @head: struct hlist_head * bucket head
 */
#define hlist_for_each_safe(pos, tmp, head)                                                        \
    for (pos = (head)->first; pos != NULL && ({                                                    \
                                  tmp = pos->next;                                                 \
                                  1;                                                               \
                              });                                                                  \
         pos = tmp)

/*
 * hlist_for_each_entry — iterate over entries in a bucket.
 *
 * @pos:    type * loop cursor
 * @head:   struct hlist_head * bucket head
 * @member: name of hlist_node field in the struct
 */
#define hlist_for_each_entry(pos, head, member)                                                    \
    for (pos = (head)->first ? hlist_entry((head)->first, typeof(*pos), member) : NULL;            \
         pos != NULL;                                                                              \
         pos = pos->member.next ? hlist_entry(pos->member.next, typeof(*pos), member) : NULL)

#endif /* HASHTABLE_H */
