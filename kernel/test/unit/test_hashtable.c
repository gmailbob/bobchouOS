/*
 * test_hashtable.c -- Tests for intrusive hash table (Round 5-0).
 */

#include "test/test.h"
#include "hashtable.h"
#include "types.h"

struct record {
    int key;
    int value;
    struct list_head hash_link;
};

#define TEST_HT_BITS 4 /* 16 buckets */

void
test_hashtable(void) {
    kprintf("[hashtable]\n");

    /* --- Initialization --- */

    DEFINE_HASHTABLE(ht, TEST_HT_BITS);
    hash_init(ht, TEST_HT_BITS);
    int all_empty = 1;
    for (int i = 0; i < HT_SIZE(TEST_HT_BITS); i++) {
        if (!list_empty(&ht[i]))
            all_empty = 0;
    }
    TEST_ASSERT(all_empty, "hash_init: all buckets empty");

    /* --- hash_add / basic insertion --- */

    struct record r1 = {.key = 10, .value = 100};
    hash_add(ht, &r1.hash_link, TEST_HT_BITS, hash_int(r1.key));

    int bucket_idx = hash_int(10) & (HT_SIZE(TEST_HT_BITS) - 1);
    TEST_ASSERT(!list_empty(&ht[bucket_idx]), "bucket non-empty after add");

    /* --- Lookup: find entry by key --- */

    int found = 0;
    struct record *pos;
    list_for_each_entry(pos, &ht[bucket_idx], hash_link) {
        if (pos->key == 10) {
            found = 1;
            TEST_ASSERT(pos->value == 100, "found entry has correct value");
        }
    }
    TEST_ASSERT(found, "lookup finds inserted entry");

    /* --- Multiple entries --- */

    struct record r2 = {.key = 20, .value = 200};
    struct record r3 = {.key = 30, .value = 300};
    struct record r4 = {.key = 40, .value = 400};

    hash_add(ht, &r2.hash_link, TEST_HT_BITS, hash_int(r2.key));
    hash_add(ht, &r3.hash_link, TEST_HT_BITS, hash_int(r3.key));
    hash_add(ht, &r4.hash_link, TEST_HT_BITS, hash_int(r4.key));

    /* All four should be findable. */
    int keys[] = {10, 20, 30, 40};
    int values[] = {100, 200, 300, 400};
    int find_count = 0;

    for (int i = 0; i < 4; i++) {
        int bi = hash_int(keys[i]) & (HT_SIZE(TEST_HT_BITS) - 1);
        list_for_each_entry(pos, &ht[bi], hash_link) {
            if (pos->key == keys[i] && pos->value == values[i])
                find_count++;
        }
    }
    TEST_ASSERT(find_count == 4, "all 4 entries findable by key");

    /* --- Deletion --- */

    hash_del(&r2.hash_link);

    /* r2 should not be found anymore. */
    found = 0;
    bucket_idx = hash_int(20) & (HT_SIZE(TEST_HT_BITS) - 1);
    list_for_each_entry(pos, &ht[bucket_idx], hash_link) {
        if (pos->key == 20)
            found = 1;
    }
    TEST_ASSERT(!found, "deleted entry not found in bucket");

    /* Others still present. */
    found = 0;
    bucket_idx = hash_int(10) & (HT_SIZE(TEST_HT_BITS) - 1);
    list_for_each_entry(pos, &ht[bucket_idx], hash_link) {
        if (pos->key == 10)
            found = 1;
    }
    TEST_ASSERT(found, "other entries survive deletion");

    /* --- Collision handling (same bucket) --- */

    DEFINE_HASHTABLE(ht2, 2); /* 4 buckets — high collision rate */
    hash_init(ht2, 2);

    struct record recs[8];
    for (int i = 0; i < 8; i++) {
        recs[i].key = i;
        recs[i].value = i * 10;
        hash_add(ht2, &recs[i].hash_link, 2, hash_int(recs[i].key));
    }

    find_count = 0;
    for (int i = 0; i < 8; i++) {
        int bi = hash_int(i) & (HT_SIZE(2) - 1);
        list_for_each_entry(pos, &ht2[bi], hash_link) {
            if (pos->key == i && pos->value == i * 10)
                find_count++;
        }
    }
    TEST_ASSERT(find_count == 8, "all 8 entries found in 4-bucket table");

    /* Delete from middle of collision chain. */
    hash_del(&recs[3].hash_link);
    hash_del(&recs[5].hash_link);

    find_count = 0;
    for (int i = 0; i < 8; i++) {
        if (i == 3 || i == 5)
            continue;
        int bi = hash_int(i) & (HT_SIZE(2) - 1);
        list_for_each_entry(pos, &ht2[bi], hash_link) {
            if (pos->key == i && pos->value == i * 10)
                find_count++;
        }
    }
    TEST_ASSERT(find_count == 6, "6 entries remain after 2 deletions");

    /* Deleted entries should be gone. */
    found = 0;
    bucket_idx = hash_int(3) & (HT_SIZE(2) - 1);
    list_for_each_entry(pos, &ht2[bucket_idx], hash_link) {
        if (pos->key == 3)
            found = 1;
    }
    TEST_ASSERT(!found, "deleted collision entry not found");

    /* --- Duplicate keys (same key, different values) --- */

    DEFINE_HASHTABLE(ht3, TEST_HT_BITS);
    hash_init(ht3, TEST_HT_BITS);

    struct record dup1 = {.key = 7, .value = 100};
    struct record dup2 = {.key = 7, .value = 200};
    hash_add(ht3, &dup1.hash_link, TEST_HT_BITS, hash_int(7));
    hash_add(ht3, &dup2.hash_link, TEST_HT_BITS, hash_int(7));

    int dup_count = 0;
    bucket_idx = hash_int(7) & (HT_SIZE(TEST_HT_BITS) - 1);
    list_for_each_entry(pos, &ht3[bucket_idx], hash_link) {
        if (pos->key == 7)
            dup_count++;
    }
    TEST_ASSERT(dup_count == 2, "duplicate keys: both entries stored");

    /* Deleting one leaves the other intact */
    hash_del(&dup1.hash_link);
    found = 0;
    list_for_each_entry(pos, &ht3[bucket_idx], hash_link) {
        if (pos->key == 7 && pos->value == 200)
            found = 1;
    }
    TEST_ASSERT(found, "duplicate keys: second survives first's deletion");

    /* --- hash_int distribution sanity check --- */

    /* Different inputs should produce different hashes. */
    uint64 h1 = hash_int(0);
    uint64 h2 = hash_int(1);
    uint64 h3 = hash_int(2);
    TEST_ASSERT(h1 != h2, "hash_int(0) != hash_int(1)");
    TEST_ASSERT(h2 != h3, "hash_int(1) != hash_int(2)");
    TEST_ASSERT(h1 != h3, "hash_int(0) != hash_int(2)");
}
