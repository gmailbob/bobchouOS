/*
 * bio.h — Buffer cache (block I/O) interface for bobchouOS.
 *
 * The layer between the file system and the virtio-blk driver. Turns
 * "transfer a block" into "borrow *the* block": at most one in-memory
 * copy of each (dev, blockno), at most one process operating on it at a
 * time (its per-buf sleep-lock).
 *
 * The file system above calls only this API — never virtio_blk_rw
 * directly. The driver is invoked by the cache on a miss.
 *
 * See Lecture 7-2.
 */

#ifndef BIO_H
#define BIO_H

#include "types.h"

struct buf;

/* Number of cache buffers (static pool). Sized above
 * max-held-per-op × concurrent-ops so bget never runs dry. */
#define NBUF 30

/* One-time boot setup: init the cache spinlock, build the LRU list,
 * init each buffer's sleep-lock. Call once before the FS is used. */
void binit(void);

/* Return *the* buffer for (dev, blockno): locked (sleep-lock held) and
 * valid (data[] holds the block — read from disk on a miss). Pair every
 * bread with a brelse. */
struct buf *bread(uint32 dev, uint32 blockno);

/* Write a locked buffer's data[] back to disk, synchronously. Caller
 * must hold b->lock. */
void bwrite(struct buf *b);

/* Release a locked buffer: drop the sleep-lock, drop the refcount, move
 * to most-recently-used. Caller must hold b->lock. */
void brelse(struct buf *b);

/* Pin/unpin: bump/drop refcnt so a buffer survives brelse without being
 * evicted. Used by the log (7-3) to hold committed-but-not-installed
 * blocks. */
void bpin(struct buf *b);
void bunpin(struct buf *b);

#endif /* BIO_H */
