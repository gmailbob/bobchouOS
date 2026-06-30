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
    struct list_head lru; /* all buffers; MRU (front) → LRU (back). Reordered on
                           * brelse (last-release ≈ last-use). */
} bcache;

/*
 * binit — one-time boot setup: init the index lock, the LRU list, and each
 * buffer's sleep-lock. All buffers start in the list, refcnt 0, invalid.
 */
void
binit(void) {
    spin_init(&bcache.lock, "bcache");
    INIT_LIST_HEAD(&bcache.lru);
    for (int i = 0; i < NBUF; i++) {
        sleep_init(&bcache.buf[i].lock, "buffer");
        list_add(&bcache.buf[i].lru_link, &bcache.lru);
    }
}

/*
 * bget — return the buffer for (dev, blockno), locked, recycling on a miss.
 *
 * In both paths: bump refcnt under bcache.lock (this reserves the buffer so
 * it can't be recycled away), release the spinlock, THEN sleep_lock — we
 * must not sleep holding a spinlock. See Lecture 7-2, Part 5.
 */
static struct buf *
bget(uint32 dev, uint32 blockno) {
    unsigned long irq;
    spin_lock_irqsave(&bcache.lock, &irq);

    /* Loop 1 — cache hit: block already resident. */
    struct buf *b;
    list_for_each_entry(b, &bcache.lru, lru_link) {
        if (b->dev == dev && b->blockno == blockno) {
            b->refcnt++;
            spin_unlock_irqrestore(&bcache.lock, irq);
            sleep_lock(&b->lock);
            return b;
        }
    }

    /* Loop 2 — miss: recycle the least-recently-used unused buffer. Scan
     * from the LRU end (tail); valid=0 forces bread to re-read from disk. */
    list_for_each_entry_reverse(b, &bcache.lru, lru_link) {
        if (b->refcnt == 0) {
            b->dev = dev;
            b->blockno = blockno;
            b->valid = 0;
            b->refcnt = 1;
            spin_unlock_irqrestore(&bcache.lock, irq);
            sleep_lock(&b->lock);
            return b;
        }
    }

    /* Every buffer is in use at once — assertion of the NBUF bound, not a
     * runtime error path. See Lecture 7-2, Subtlety 4. */
    panic("bget: no buffers");
}

/*
 * bread — return a locked buffer holding the block's data.
 *
 * Cache hit (valid): pure memory. Miss: read from disk once, then mark
 * valid so future breads skip the disk until this buffer is recycled.
 */
struct buf *
bread(uint32 dev, uint32 blockno) {
    struct buf *b = bget(dev, blockno);
    if (!b->valid) {
        b->req.type = VIRTIO_BLK_T_IN; /* read direction for the driver */
        virtio_blk_rw(b);
        b->valid = 1;
    }
    return b;
}

/*
 * bwrite — flush a locked buffer's data to disk, synchronously.
 * Caller must hold b->lock (it came from bread).
 */
void
bwrite(struct buf *b) {
    if (!sleep_holding(&b->lock))
        panic("bwrite: buf not locked");
    b->req.type = VIRTIO_BLK_T_OUT; /* write direction for the driver */
    virtio_blk_rw(b);
}

/*
 * brelse — release a locked buffer: drop the data lock, drop the ref.
 *
 * Lock order is the reverse of bget: release the sleeplock first, then take
 * bcache.lock to touch refcnt and the LRU list. When the last ref drops,
 * move the buffer to the MRU end (front) — see "Why LRU" in Lecture 7-2.
 */
void
brelse(struct buf *b) {
    if (!sleep_holding(&b->lock))
        panic("brelse: buf not locked");
    sleep_unlock(&b->lock);

    unsigned long irq;
    spin_lock_irqsave(&bcache.lock, &irq);
    if (--b->refcnt == 0) { /* no users left → now an evictable cache entry */
        list_del(&b->lru_link);
        list_add(&b->lru_link, &bcache.lru); /* front = most-recently-used */
    }
    spin_unlock_irqrestore(&bcache.lock, irq);
}

/*
 * bpin / bunpin — adjust refcnt so a buffer survives brelse (used by the
 * 7-3 log). No sleeplock: pinning is about eviction eligibility, not data.
 */
void
bpin(struct buf *b) {
    unsigned long irq;
    spin_lock_irqsave(&bcache.lock, &irq);
    b->refcnt++;
    spin_unlock_irqrestore(&bcache.lock, irq);
}

void
bunpin(struct buf *b) {
    unsigned long irq;
    spin_lock_irqsave(&bcache.lock, &irq);
    b->refcnt--;
    spin_unlock_irqrestore(&bcache.lock, irq);
}
