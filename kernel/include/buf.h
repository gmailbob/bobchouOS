/*
 * buf.h — In-memory block buffer.
 *
 * A struct buf is the unit of disk I/O: one BSIZE-byte block plus the
 * metadata the driver needs to perform a transfer. Introduced minimally
 * in Round 7-1 (just the driver fields); Round 7-2 grows it with
 * buffer-cache fields (valid, refcnt, sleep-lock, LRU link) — purely
 * additive, no existing field changes.
 *
 * Design decision (Lecture 7-1, Part 5): the virtio request header and
 * status byte live INSIDE struct buf (Linux bio-style), not in a side
 * array. The buf IS the I/O request — it stays alive for the entire
 * duration of the transfer, so the device can DMA into it safely.
 *
 * See Lecture 7-1.
 */

#ifndef BUF_H
#define BUF_H

#include "types.h"
#include "virtio.h"
#include "sleeplock.h"
#include "list.h"

/* Block size: matches PG_SIZE (4096). One block = one page = one DMA
 * transfer. This constant propagates through the whole filesystem
 * (cache, log, bitmap, inodes, mkfs) — see Lecture 7-1, Part 6. */
#define BSIZE 4096

struct buf {
    uint32 dev;        /* device number (one device for now) */
    uint32 blockno;    /* logical block number on disk */
    uint8 data[BSIZE]; /* the block's contents */

    /* --- virtio driver fields (embedded request metadata) --- */
    struct virtio_blk_req req; /* request header (type + sector) */
    uint8 status;              /* device writes result: 0=ok, 1=err, 2=unsupp */
    int disk;                  /* 1 = in-flight (device owns buf), 0 = complete */

    /* --- buffer-cache fields (Round 7-2, additive) --- */
    int valid;                 /* has data[] been read from disk? */
    int refcnt;                /* # bread holders + pins (0 = evictable) */
    struct sleeplock lock;     /* held bread → brelse (guards data[]/valid) */
    struct list_head lru_link; /* node in the bcache LRU list */
};

#endif /* BUF_H */
