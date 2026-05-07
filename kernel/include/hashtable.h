/*
 * hashtable.h — Intrusive hash table with separate chaining (list_head buckets).
 *
 * See Lecture 5-0 for design rationale (why separate chaining, multiplicative
 * hashing, collision handling, and how Linux differs with hlist).
 *
 * Each bucket is a standard list_head sentinel — same primitive as all other
 * lists in the kernel.
 *
 * == Public API ==
 *
 *   Table setup:
 *     DEFINE_HASHTABLE(name, bits)       declare + init (2^bits buckets)
 *     HT_SIZE(bits)                      number of buckets
 *     hash_init(ht, bits)                runtime init
 *
 *   Insertion / removal:
 *     hash_add(ht, node, bits, hash)     insert into computed bucket
 *     hash_del(node)                     remove from table, O(1)
 *
 *   Hash function:
 *     hash_int(val)                      multiplicative hash for integers
 *
 *   Iteration (within one bucket):
 *     Use list_for_each_entry on &ht[bucket] — no special macros needed.
 *
 * == Usage ==
 *
 *   // 1. Embed a list_head in your struct for hash membership:
 *   struct proc {
 *       int pid;
 *       struct list_head pid_link;  // hash table bucket chain
 *   };
 *
 *   // 2. Declare a hash table:
 *   #define PID_HASH_BITS 6              // 64 buckets
 *   DEFINE_HASHTABLE(pid_table, PID_HASH_BITS);  // static init
 *   // or at runtime:
 *   struct list_head pid_table[HT_SIZE(PID_HASH_BITS)];
 *   hash_init(pid_table, PID_HASH_BITS);
 *
 *   // 3. Insert:
 *   struct proc p = { .pid = 42 };
 *   hash_add(pid_table, &p.pid_link, PID_HASH_BITS, hash_int(p.pid));
 *
 *   // 4. Lookup by key (iterate the bucket, compare keys):
 *   int bucket = hash_int(42) & (HT_SIZE(PID_HASH_BITS) - 1);
 *   struct proc *found;
 *   list_for_each_entry(found, &pid_table[bucket], pid_link) {
 *       if (found->pid == 42)
 *           break;  // got it
 *   }
 *
 *   // 5. Remove (O(1), same as any list_del):
 *   hash_del(&p.pid_link);
 */

#ifndef HASHTABLE_H
#define HASHTABLE_H

#include "types.h"
#include "list.h"

/*
 * HT_SIZE — number of buckets for a given bit count.
 */
#define HT_SIZE(bits) (1 << (bits))

/*
 * DEFINE_HASHTABLE — declare and initialize a hash table array.
 * Each bucket is a list_head initialized to empty (points to itself).
 *
 * @name: variable name
 * @bits: log2 of number of buckets (e.g., 4 -> 16 buckets)
 */
#define DEFINE_HASHTABLE(name, bits) struct list_head name[HT_SIZE(bits)]

/*
 * hash_init — runtime initialization (INIT_LIST_HEAD each bucket).
 *
 * @ht:   the hash table array
 * @bits: log2 of number of buckets
 */
static inline void
hash_init(struct list_head *ht, int bits) {
    for (int i = 0; i < HT_SIZE(bits); i++) {
        INIT_LIST_HEAD(&ht[i]);
    }
}

/*
 * hash_add — insert entry into hash table at bucket determined by key hash.
 *
 * @ht:   the hash table array
 * @node: the list_head embedded in the entry (for hash chain membership)
 * @bits: log2 of number of buckets
 * @hash: pre-computed hash value of the key
 */
static inline void
hash_add(struct list_head *ht, struct list_head *node, int bits, uint64 hash) {
    list_add(node, &ht[hash & (HT_SIZE(bits) - 1)]);
}

/*
 * hash_del — remove an entry from the hash table.
 * Same as list_del — just a wrapper for symmetry with hash_add.
 */
static inline void
hash_del(struct list_head *node) {
    list_del(node);
}

/*
 * hash_int — simple integer hash (multiplicative hashing).
 * Good enough for PID lookup. Returns a full 64-bit hash;
 * caller masks to the needed bits.
 */
static inline uint64
hash_int(uint64 val) {
    return val * 0x61C8864680B583EBull;
}

#endif /* HASHTABLE_H */
