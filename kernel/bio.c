/*
 * bio.c — Buffer cache implementation.
 *
 * A fixed pool of NBUF buffers behind a single spinlock (the index) plus
 * one sleep-lock per buffer (the data). bget does lookup-or-recycle with
 * the classic xv6 two-loop structure; brelse maintains LRU order.
 *
 * Two locks, two jobs (Lecture 7-2, Part 4):
 *   bcache.lock (spinlock) — guards the buffer SET: the LRU list, every
 *                            refcnt, the hit/miss decision. Held nanoseconds.
 *   b->lock     (sleeplock) — guards one buffer's CONTENTS: data[], valid.
 *                            Held the whole bread → brelse span, across I/O.
 *
 * Lock ordering: bcache.lock first, released BEFORE sleep_lock (you must
 * never sleep holding a spinlock). See Lecture 7-2, Parts 5–7.
 */

#include "bio.h"
#include "buf.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "list.h"
#include "virtio.h"
#include "drivers/virtio_blk.h"
#include "kprintf.h"

/* The cache: a static buffer pool, one spinlock guarding the index, and
 * the LRU list (MRU at front, LRU at back). */
static struct {
    struct spinlock lock;
    struct buf buf[NBUF];
    struct list_head lru; /* all buffers; ordered MRU (front) → LRU (back) */
} bcache;

/*
 * binit — one-time boot setup.
 *
 * TODO(you):
 *   - spin_init(&bcache.lock, "bcache")
 *   - INIT_LIST_HEAD(&bcache.lru)
 *   - for each buf: sleep_init(&b->lock, "buffer") and add it to the LRU
 *     list (list_add) — all start refcnt==0, valid==0.
 */
void
binit(void) {
    /* TODO */
}

/*
 * bget — return the locked buffer for (dev, blockno), recycling if needed.
 *
 * Two loops under bcache.lock:
 *   Loop 1 (hit): scan for a buf already caching (dev, blockno). If found,
 *     bump refcnt, RELEASE bcache.lock, THEN sleep_lock(&b->lock), return.
 *   Loop 2 (miss): scan the LRU list in REVERSE for a refcnt==0 victim.
 *     Claim it (set dev/blockno, valid=0, refcnt=1), release bcache.lock,
 *     sleep_lock, return.
 *   If neither loop finds one: panic("bget: no buffers") — an assertion
 *     that the static bound held (Lecture 7-2, Subtlety 4).
 *
 * Why release the spinlock before sleep_lock: sleep_lock may sleep, and
 * you must not sleep holding a spinlock. The refcnt++ done under the
 * spinlock reserves the buffer so it can't be recycled out from under us.
 *
 * TODO(you): implement both loops. Useful helpers:
 *   list_for_each_entry(b, &bcache.lru, lru_link)          — forward (hits)
 *   list_for_each_entry_reverse(b, &bcache.lru, lru_link)  — reverse (LRU)
 */
static struct buf *
bget(uint32 dev, uint32 blockno) {
    /* TODO */
    return 0;
}

/*
 * bread — bget plus "fill from disk on a miss".
 *
 * TODO(you):
 *   - b = bget(dev, blockno)
 *   - if (!b->valid): set b->req.type = VIRTIO_BLK_T_IN (read direction),
 *     virtio_blk_rw(b), then b->valid = 1.
 *   - return b (locked, valid).
 */
struct buf *
bread(uint32 dev, uint32 blockno) {
    /* TODO */
    return 0;
}

/*
 * bwrite — flush a locked buffer to disk synchronously.
 *
 * TODO(you):
 *   - assert sleep_holding(&b->lock) else panic("bwrite: buf not locked")
 *   - set b->req.type = VIRTIO_BLK_T_OUT (write direction)
 *   - virtio_blk_rw(b)
 */
void
bwrite(struct buf *b) {
    /* TODO */
}

/*
 * brelse — release a locked buffer.
 *
 * Mirror of bget's lock dance, reversed: release the SLEEPLOCK first,
 * then take the SPINLOCK to touch refcnt and the LRU list.
 *
 * TODO(you):
 *   - assert sleep_holding(&b->lock) else panic("brelse: buf not locked")
 *   - sleep_unlock(&b->lock)
 *   - under bcache.lock: b->refcnt--; if it hit 0, move b to the FRONT of
 *     the LRU list (list_del then list_add) — most-recently-used.
 */
void
brelse(struct buf *b) {
    /* TODO */
}

/*
 * bpin / bunpin — adjust refcnt so a buffer survives brelse (used by the
 * 7-3 log). No sleeplock: pinning is about eviction eligibility, not data.
 *
 * TODO(you): under bcache.lock, bump (bpin) / drop (bunpin) b->refcnt.
 */
void
bpin(struct buf *b) {
    /* TODO */
}

void
bunpin(struct buf *b) {
    /* TODO */
}
