/*
 * test_bio.c — Integration test for the buffer cache.
 *
 * Needs the scheduler + a live virtio-blk device: bread sleeps on a miss
 * waiting for the disk. Replaces the 7-1 virtio smoke-test scaffolding
 * with a real read path through the cache.
 *
 * The disk image (fs.img) is zeroed until mkfs in 7-4, so we assert on
 * cache *behavior* (hit/miss, buffer identity, validity, eviction,
 * refcnt) rather than on data contents.
 *
 * See Lecture 7-2.
 */

#include "test/test.h"
#include "bio.h"
#include "buf.h"

#define TEST_DEV 0

void
test_bio(void) {
    kprintf("[test_bio]\n");

    /* --- Miss then hit: same block returns the same buffer --- */

    struct buf *b1 = bread(TEST_DEV, 0);
    TEST_ASSERT(b1 != 0, "bread(0): returns a buffer");
    TEST_ASSERT(b1->valid == 1, "bread(0): buffer is valid (read from disk)");
    TEST_ASSERT(b1->blockno == 0, "bread(0): blockno set");
    TEST_ASSERT(b1->refcnt >= 1, "bread(0): refcnt held");
    brelse(b1);

    /* Re-read block 0: should hit the cache and hand back the SAME buf,
     * still valid (no second disk read needed). */
    struct buf *b2 = bread(TEST_DEV, 0);
    TEST_ASSERT(b2 == b1, "bread(0) again: cache hit, same struct buf");
    TEST_ASSERT(b2->valid == 1, "cache hit: still valid");
    brelse(b2);

    /* --- Distinct blocks get distinct buffers --- */

    struct buf *ba = bread(TEST_DEV, 1);
    struct buf *bb = bread(TEST_DEV, 2);
    TEST_ASSERT(ba != bb, "blocks 1 and 2 map to different buffers");
    TEST_ASSERT(ba->blockno == 1, "buffer a caches block 1");
    TEST_ASSERT(bb->blockno == 2, "buffer b caches block 2");
    brelse(ba);
    brelse(bb);

    /* --- refcnt: a held buffer is not recycled out from under us --- */

    struct buf *held = bread(TEST_DEV, 3);
    TEST_ASSERT(held->refcnt >= 1, "held buffer has refcnt >= 1");
    /* Touch many other blocks while `held` is still checked out. Even if
     * eviction runs, block 3's buffer must not be repurposed. */
    for (uint32 blk = 10; blk < 10 + NBUF; blk++) {
        struct buf *t = bread(TEST_DEV, blk);
        brelse(t);
    }
    TEST_ASSERT(held->blockno == 3, "held buffer still caches block 3 after churn");
    TEST_ASSERT(held->valid == 1, "held buffer still valid after churn");
    brelse(held);

    /* --- Eviction: recycling a buffer re-reads (valid was reset) --- */

    /* Block 0 was almost certainly evicted by the churn above. Re-reading
     * it must still return a valid buffer (re-read from disk, not stale). */
    struct buf *b0 = bread(TEST_DEV, 0);
    TEST_ASSERT(b0->blockno == 0, "re-read block 0 after eviction");
    TEST_ASSERT(b0->valid == 1, "re-read block 0: valid (fetched again)");
    brelse(b0);
}
