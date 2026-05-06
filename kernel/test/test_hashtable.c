/*
 * test_hashtable.c -- Tests for intrusive hash table (Round 5-0).
 */

#include "test/test.h"
#include "hashtable.h"
#include "types.h"

struct record {
    int key;
    int value;
    struct hlist_node hash_link;
};

#define TEST_HT_BITS 4 /* 16 buckets */

void
test_hashtable(void) {
    kprintf("[hashtable]\n");

    /* --- Initialization --- */

    DEFINE_HASHTABLE(ht, TEST_HT_BITS);
    int all_null = 1;
    for (int i = 0; i < HT_SIZE(TEST_HT_BITS); i++) {
        if (ht[i].first != NULL)
            all_null = 0;
    }
    TEST_ASSERT(all_null, "DEFINE_HASHTABLE: all buckets NULL");

    /* --- hlist_unhashed --- */

    struct record r1 = {.key = 10, .value = 100};
    r1.hash_link.next = NULL;
    r1.hash_link.pprev = NULL;
    TEST_ASSERT(hlist_unhashed(&r1.hash_link), "node starts unhashed");

    /* --- hash_add / basic insertion --- */

    hash_add(ht, &r1.hash_link, TEST_HT_BITS, hash_int(r1.key));
    TEST_ASSERT(!hlist_unhashed(&r1.hash_link), "node is hashed after add");

    /* --- Lookup: find entry by key --- */

    int found = 0;
    uint64 bucket_idx = hash_int(10) & (HT_SIZE(TEST_HT_BITS) - 1);
    struct hlist_node *pos;
    hlist_for_each(pos, &ht[bucket_idx]) {
        struct record *r = hlist_entry(pos, struct record, hash_link);
        if (r->key == 10) {
            found = 1;
            TEST_ASSERT(r->value == 100, "found entry has correct value");
        }
    }
    TEST_ASSERT(found, "lookup finds inserted entry");

    /* --- Multiple entries --- */

    struct record r2 = {.key = 20, .value = 200, .hash_link = {NULL, NULL}};
    struct record r3 = {.key = 30, .value = 300, .hash_link = {NULL, NULL}};
    struct record r4 = {.key = 40, .value = 400, .hash_link = {NULL, NULL}};

    hash_add(ht, &r2.hash_link, TEST_HT_BITS, hash_int(r2.key));
    hash_add(ht, &r3.hash_link, TEST_HT_BITS, hash_int(r3.key));
    hash_add(ht, &r4.hash_link, TEST_HT_BITS, hash_int(r4.key));

    /* All four should be findable. */
    int keys[] = {10, 20, 30, 40};
    int values[] = {100, 200, 300, 400};
    int find_count = 0;

    for (int i = 0; i < 4; i++) {
        uint64 bi = hash_int(keys[i]) & (HT_SIZE(TEST_HT_BITS) - 1);
        hlist_for_each(pos, &ht[bi]) {
            struct record *r = hlist_entry(pos, struct record, hash_link);
            if (r->key == keys[i] && r->value == values[i])
                find_count++;
        }
    }
    TEST_ASSERT(find_count == 4, "all 4 entries findable by key");

    /* --- Deletion --- */

    hash_del(&r2.hash_link);
    TEST_ASSERT(hlist_unhashed(&r2.hash_link), "node unhashed after del");

    /* r2 should not be found anymore. */
    found = 0;
    bucket_idx = hash_int(20) & (HT_SIZE(TEST_HT_BITS) - 1);
    hlist_for_each(pos, &ht[bucket_idx]) {
        struct record *r = hlist_entry(pos, struct record, hash_link);
        if (r->key == 20)
            found = 1;
    }
    TEST_ASSERT(!found, "deleted entry not found in bucket");

    /* Others still present. */
    found = 0;
    bucket_idx = hash_int(10) & (HT_SIZE(TEST_HT_BITS) - 1);
    hlist_for_each(pos, &ht[bucket_idx]) {
        struct record *r = hlist_entry(pos, struct record, hash_link);
        if (r->key == 10)
            found = 1;
    }
    TEST_ASSERT(found, "other entries survive deletion");

    /* --- Collision handling (same bucket) --- */

    /*
     * Force entries into the same bucket by choosing keys that
     * hash to the same bucket index with 16 buckets.
     * We just insert many and verify all are retrievable.
     */
    DEFINE_HASHTABLE(ht2, 2); /* 4 buckets — high collision rate */

    struct record recs[8];
    for (int i = 0; i < 8; i++) {
        recs[i].key = i;
        recs[i].value = i * 10;
        recs[i].hash_link.next = NULL;
        recs[i].hash_link.pprev = NULL;
        hash_add(ht2, &recs[i].hash_link, 2, hash_int(recs[i].key));
    }

    find_count = 0;
    for (int i = 0; i < 8; i++) {
        uint64 bi = hash_int(i) & (HT_SIZE(2) - 1);
        hlist_for_each(pos, &ht2[bi]) {
            struct record *r = hlist_entry(pos, struct record, hash_link);
            if (r->key == i && r->value == i * 10)
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
        uint64 bi = hash_int(i) & (HT_SIZE(2) - 1);
        hlist_for_each(pos, &ht2[bi]) {
            struct record *r = hlist_entry(pos, struct record, hash_link);
            if (r->key == i && r->value == i * 10)
                find_count++;
        }
    }
    TEST_ASSERT(find_count == 6, "6 entries remain after 2 deletions");

    /* Deleted entries should be gone. */
    found = 0;
    bucket_idx = hash_int(3) & (HT_SIZE(2) - 1);
    hlist_for_each(pos, &ht2[bucket_idx]) {
        struct record *r = hlist_entry(pos, struct record, hash_link);
        if (r->key == 3)
            found = 1;
    }
    TEST_ASSERT(!found, "deleted collision entry not found");

    /* --- hash_int distribution sanity check --- */

    /* Different inputs should produce different hashes. */
    uint64 h1 = hash_int(0);
    uint64 h2 = hash_int(1);
    uint64 h3 = hash_int(2);
    TEST_ASSERT(h1 != h2, "hash_int(0) != hash_int(1)");
    TEST_ASSERT(h2 != h3, "hash_int(1) != hash_int(2)");
    TEST_ASSERT(h1 != h3, "hash_int(0) != hash_int(2)");
}
