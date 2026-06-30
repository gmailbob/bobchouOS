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
 * binit — one-time boot setup.
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
 * bget — return the locked buffer for (dev, blockno), recycling if needed.
 */
static struct buf *
bget(uint32 dev, uint32 blockno) {
    unsigned long irq;
    spin_lock_irqsave(&bcache.lock, &irq);

    struct buf *b;
    list_for_each_entry(b, &bcache.lru, lru_link) {
        if (b->dev == dev && b->blockno == blockno) {
            b->refcnt++;
            spin_unlock_irqrestore(&bcache.lock, irq);
            sleep_lock(&b->lock);
            return b;
        }
    }

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

    panic("bget: no buffers");
}

/*
 * bread — bget plus "fill from disk on a miss".
 */
struct buf *
bread(uint32 dev, uint32 blockno) {
    struct buf *b = bget(dev, blockno);
    if (!b->valid) {
        b->req.type = VIRTIO_BLK_T_IN;
        virtio_blk_rw(b);
        b->valid = 1;
    }
    return b;
}

/*
 * bwrite — flush a locked buffer to disk synchronously.
 */
void
bwrite(struct buf *b) {
    if (!sleep_holding(&b->lock))
        panic("bwrite: buf not locked");
    b->req.type = VIRTIO_BLK_T_OUT;
    virtio_blk_rw(b);
}

/*
 * brelse — release a locked buffer.
 */
void
brelse(struct buf *b) {
    if (!sleep_holding(&b->lock))
        panic("brelse: buf not locked");
    sleep_unlock(&b->lock);

    unsigned long irq;
    spin_lock_irqsave(&bcache.lock, &irq);
    if (--b->refcnt == 0) {
        list_del(&b->lru_link);
        list_add(&b->lru_link, &bcache.lru);
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
