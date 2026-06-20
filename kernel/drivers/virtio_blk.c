/*
 * virtio_blk.c — VirtIO block device driver (virtio-mmio transport).
 *
 * Talks to QEMU's virtio-blk device at VIRTIO0. The driver owns one
 * virtqueue (NUM_DESC descriptors) guarded by a single spinlock, and
 * exposes a synchronous read/write built on an async submit primitive.
 * (Round 7-1 only exercises reads; the write path is wired but the
 * direction is hardcoded to read until the buffer cache in 7-2.)
 *
 * Data path for one I/O (Lecture 7-1, Parts 5/7/8):
 *   virtio_blk_rw(b)
 *     → virtio_blk_submit(b): build a 3-descriptor chain
 *         (header → data → status), publish to the avail ring, ring the
 *         doorbell; set b->disk = 1
 *     → sleep until b->disk == 0
 *   ... device performs DMA, posts to the used ring, fires IRQ ...
 *   virtio_blk_intr(): reap the used ring, mark bufs done, wake sleepers
 *
 * See Lecture 7-1 for the full design, the register map, and the
 * design decisions (single queue, embedded request, BSIZE=4096).
 */

#include "drivers/virtio_blk.h"
#include "buf.h"
#include "virtio.h"
#include "kalloc.h"
#include "mem_layout.h"
#include "riscv.h"
#include "spinlock.h"
#include "string.h"
#include "wait_queue.h"
#include "kprintf.h"

/* ---- MMIO register access ----
 * All virtio-mmio registers are 32-bit. R is the register offset from
 * the device base (one of the VIRTIO_MMIO_* constants). */
#define R(off) ((volatile uint32 *)(VIRTIO0_BASE + (off)))

static inline void
virtio_write(uint32 off, uint32 val) {
    *R(off) = val;
}

static inline uint32
virtio_read(uint32 off) {
    return *R(off);
}

/* Write a 64-bit physical address as a LOW/HIGH register pair. `low` is
 * the offset of the LOW half; HIGH is assumed at low+4 (the virtio-mmio
 * convention). Pointers are physical addresses here (identity-mapped). */
static inline void
virtio_write_addr(uint32 low, void *ptr) {
    uint64 pa = (uint64)ptr;
    virtio_write(low, (uint32)pa);
    virtio_write(low + 4, (uint32)(pa >> 32));
}

/* ---- Driver state ----
 *
 * The three virtqueue structures live in DMA-able memory (kalloc gives
 * page-aligned pages; the device reads them by physical address). We
 * track in-flight bufs and a free-descriptor list alongside them.
 *
 * All of this is protected by disk.lock. */
static struct {
    /* Virtqueue structures (allocated in virtio_blk_init). */
    struct virtq_desc *desc;   /* descriptor table (NUM_DESC entries) */
    struct virtq_avail *avail; /* available ring (driver → device) */
    struct virtq_used *used;   /* used ring (device → driver) */

    /* The last used-ring index we have processed. The device's
     * used->idx runs ahead of this as it completes requests; the
     * interrupt handler consumes [used_idx, used->idx). */
    uint16 used_idx;

    /* Per-descriptor bookkeeping, both indexed 0..NUM_DESC-1:
     *   free[i] == 1 → descriptor i is available (simple boolean array;
     *                  see alloc_desc/free_desc below).
     *   info[i]      → the buf that owns the chain headed by descriptor i.
     *                  The used ring reports completions by head-descriptor
     *                  number, so the handler does b = info[id] to map a
     *                  completed chain back to its buf and wake it. */
    int free[NUM_DESC];
    struct buf *info[NUM_DESC];

    /* Synchronization */
    struct spinlock lock;
    struct wait_queue wq; /* bufs sleep here waiting for completion */
} disk;

/* ====================================================================
 * Descriptor allocation
 * ==================================================================== */

/*
 * count_free — number of currently-free descriptors.
 *
 * Caller holds disk.lock.
 */
static int
count_free(void) {
    int count = 0;
    for (int i = 0; i < NUM_DESC; i++) {
        if (disk.free[i])
            count++;
    }
    return count;
}

/*
 * alloc_desc — claim one free descriptor.
 *
 * Returns its index, or -1 if none are free. Caller holds disk.lock.
 */
static int
alloc_desc(void) {
    for (int i = 0; i < NUM_DESC; i++) {
        if (disk.free[i]) {
            disk.free[i] = 0;
            return i;
        }
    }
    return -1;
}

/*
 * free_desc — return one descriptor to the free list.
 *
 * Caller holds disk.lock.
 */
static void
free_desc(int i) {
    if (disk.free[i])
        panic("free_desc: attempt to double free disk.free[%d]", i);
    disk.free[i] = 1;
}

/*
 * free_chain — free a whole descriptor chain starting at head `i`.
 *
 * Walks the chain via the NEXT flag / next field, freeing each link.
 * Caller holds disk.lock.
 */
static void
free_chain(int i) {
    for (;;) {
        /* Read next/flags BEFORE freeing (free_desc may later clear the
         * descriptor). */
        int has_next = disk.desc[i].flags & VIRTQ_DESC_F_NEXT;
        int next = disk.desc[i].next;
        free_desc(i);
        if (!has_next)
            break;
        i = next;
    }
}

/* ====================================================================
 * Device initialization (8-step handshake — Lecture 7-1, Part 3)
 * ==================================================================== */

/*
 * virtio_blk_init — bring the device up and ready queue 0.
 *
 * Runs once at boot, single-threaded, before the scheduler starts —
 * no locking needed here beyond initializing disk.lock/disk.wq for
 * later use. panics if the device is absent or rejects our setup;
 * there is no graceful degradation without a disk.
 */
void
virtio_blk_init(void) {
    spin_init(&disk.lock, "virtio_blk");
    wq_init(&disk.wq, "virtio_blk");

    /* Step 1: verify the device is present and is a block device. */
    if (virtio_read(VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MAGIC ||
        virtio_read(VIRTIO_MMIO_VERSION) != VIRTIO_VERSION ||
        virtio_read(VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEV_BLOCK)
        panic("virtio_blk_init: no virtio block device at VIRTIO0");

    /* Steps 2-6: the status handshake + feature negotiation. */
    uint32 status = 0;
    virtio_write(VIRTIO_MMIO_STATUS, status); /* reset */
    status |= VIRTIO_STATUS_ACKNOWLEDGE;
    virtio_write(VIRTIO_MMIO_STATUS, status);
    status |= VIRTIO_STATUS_DRIVER;
    virtio_write(VIRTIO_MMIO_STATUS, status);

    /* Negotiate nothing: accept zero features. The feature space spans
     * two 32-bit words (bits 0..31 and 32..63), reached via the *_SEL
     * selector; "accept nothing" must zero BOTH words. (See Lecture 7-1,
     * "Feature negotiation".) */
    for (uint32 word = 0; word < 2; word++) {
        virtio_write(VIRTIO_MMIO_DRIVER_FEATURES_SEL, word);
        virtio_write(VIRTIO_MMIO_DRIVER_FEATURES, 0);
    }

    status |= VIRTIO_STATUS_FEATURES_OK;
    virtio_write(VIRTIO_MMIO_STATUS, status);

    if (!(virtio_read(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK))
        panic("virtio_blk_init: FEATURES_OK cannot be set");

    /* Step 7: set up queue 0. */
    virtio_write(VIRTIO_MMIO_QUEUE_SEL, 0);

    uint32 num_max = virtio_read(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (num_max == 0)
        panic("virtio_blk_init: queue unavailable");
    if (num_max < NUM_DESC)
        panic("virtio_blk_init: device depth=%d < %d=NUM_DESC", num_max, NUM_DESC);

    /* One page each, not kmalloc: the device DMAs these by physical
     * address with hardware alignment requirements. A page (4096-aligned)
     * satisfies every layout — including the legacy spec's page-aligned
     * virtqueue — without coupling us to slab-slot alignment. The ~12 KB
     * is a one-time boot cost, not a per-I/O allocation. */
    if (!(disk.desc = kalloc()) || !(disk.avail = kalloc()) || !(disk.used = kalloc()))
        panic("virtio_blk_init: failed to kalloc desc, avail or used");

    /* Mark all descriptors free. (disk is BSS-zeroed, so free[] starts
     * all-0 = "not free".) */
    for (int i = 0; i < NUM_DESC; i++)
        disk.free[i] = 1;

    virtio_write(VIRTIO_MMIO_QUEUE_NUM, NUM_DESC);

    virtio_write_addr(VIRTIO_MMIO_QUEUE_DESC_LOW, disk.desc);
    virtio_write_addr(VIRTIO_MMIO_QUEUE_DRIVER_LOW, disk.avail);
    virtio_write_addr(VIRTIO_MMIO_QUEUE_DEVICE_LOW, disk.used);

    virtio_write(VIRTIO_MMIO_QUEUE_READY, 1); /* queue 0 is now live */

    /* Step 8: tell the device we're live — set DRIVER_OK in STATUS. */
    status |= VIRTIO_STATUS_DRIVER_OK;
    virtio_write(VIRTIO_MMIO_STATUS, status);
    kprintf("virtio_blk: init done\n");
}

/* ====================================================================
 * Request submission (Lecture 7-1, Part 7)
 * ==================================================================== */

/*
 * virtio_blk_submit — post one I/O request and return immediately.
 *
 * Builds the 3-descriptor chain (header → data → status), publishes it
 * to the avail ring, and rings the doorbell. Does NOT wait — the caller
 * sleeps on b->disk (see virtio_blk_rw).
 *
 * Takes disk.lock with irqsave: the lock is shared with virtio_blk_intr,
 * so the completion interrupt must stay masked while we hold it, or it
 * would self-deadlock on this same hart. (Lecture 7-1, Part 9.)
 */
void
virtio_blk_submit(struct buf *b) {
    unsigned long irq;
    spin_lock_irqsave(&disk.lock, &irq);

    /* Allocate three descriptors (d0=header, d1=data, d2=status). The
     * count check and the three allocs share one critical section (no
     * unlock between them), so once we see >= 3 free the allocs can't
     * fail. */
    while (count_free() < 3)
        wq_sleep(&disk.wq, &disk.lock);
    int d0 = alloc_desc();
    int d1 = alloc_desc();
    int d2 = alloc_desc();

    /* Fill the request header in the buf. The DIRECTION now comes from the
     * caller: bread sets b->req.type = VIRTIO_BLK_T_IN, bwrite sets
     * VIRTIO_BLK_T_OUT (Round 7-2). The descriptor flags below key off it.
     *
     * TODO(you, 7-2): derive `write` from b->req.type instead of hardcoding.
     *   int write = (b->req.type == VIRTIO_BLK_T_OUT);
     * (Round 7-1 hardcoded write=0 for the block-0 read smoke test.) */
    int write = 0; /* TODO: write = (b->req.type == VIRTIO_BLK_T_OUT); */
    b->req.reserved = 0;
    b->req.sector = (uint64)b->blockno * SECTORS_PER_BLOCK; /* 64-bit math */

    /* Fill descriptor d0 (device reads) */
    disk.desc[d0].addr = (uint64)&b->req;
    disk.desc[d0].len = sizeof(b->req);
    disk.desc[d0].flags = VIRTQ_DESC_F_NEXT;
    disk.desc[d0].next = d1;

    /* Fill descriptor d1 (data) */
    disk.desc[d1].addr = (uint64)b->data;
    disk.desc[d1].len = BSIZE;
    disk.desc[d1].flags = VIRTQ_DESC_F_NEXT | (write ? VIRTQ_DESC_F_WRITE : 0);
    disk.desc[d1].next = d2;

    /* Fill descriptor d2 (status, device writes one byte, end of chain) */
    disk.desc[d2].addr = (uint64)&b->status;
    disk.desc[d2].len = 1;
    disk.desc[d2].flags = VIRTQ_DESC_F_WRITE;
    disk.desc[d2].next = 0;

    /* Record the chain owner (info[d0] → b) and mark the buf in-flight.
     * b->disk is the completion flag: 1 = I/O outstanding. virtio_blk_rw
     * sleeps while it's 1; virtio_blk_intr clears it to 0 on completion
     * and wakes the sleeper. */
    disk.info[d0] = b;
    b->disk = 1;

    /* Publish to the avail ring, then bump idx. (The ring needed no
     * explicit init: kalloc zeroed disk.avail's page, and flags=idx=0 is
     * exactly an empty ring — the shared zero-start we and the device assume.) */
    disk.avail->ring[disk.avail->idx % NUM_DESC] = d0;
    __sync_synchronize();
    disk.avail->idx += 1;
    __sync_synchronize();

    /* Ring the doorbell. 0 refers to queue 0 */
    virtio_write(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    spin_unlock_irqrestore(&disk.lock, irq);
}

/* ====================================================================
 * Synchronous wrapper (Lecture 7-1, Part 9)
 * ==================================================================== */

/*
 * virtio_blk_rw — submit, then block until the I/O completes.
 *
 * The synchronous face of the driver (what the buffer cache will call):
 * submit returns immediately, then we sleep until the interrupt handler
 * clears b->disk. The condition loop re-checks after each wake, since a
 * wakeup may be for a different buf's completion.
 */
void
virtio_blk_rw(struct buf *b) {
    virtio_blk_submit(b);

    unsigned long irq;
    spin_lock_irqsave(&disk.lock, &irq);
    while (b->disk == 1)
        wq_sleep(&disk.wq, &disk.lock);
    spin_unlock_irqrestore(&disk.lock, irq);

    /* The device wrote its result into b->status. We have no way to
     * recover from a failed disk I/O at this layer, so a non-OK status
     * is fatal. (Round 7-2's buffer cache may surface errors to callers
     * instead of panicking.) */
    if (b->status != VIRTIO_BLK_S_OK)
        panic("virtio_blk_rw: block %d failed, status=%d", b->blockno, b->status);
}

/* ====================================================================
 * Interrupt handler — top half (Lecture 7-1, Part 8)
 * ==================================================================== */

/*
 * virtio_blk_intr — reap completions and wake their sleepers.
 *
 * Called from the trap path when the PLIC reports a virtio-blk IRQ.
 * Runs in interrupt context (interrupts already disabled by trap entry),
 * so it takes disk.lock with the plain spin_lock — there is no interrupt
 * state to save. Does only the "top half" work: mark bufs done, free
 * their descriptors, wake the waiters. The real work happens in the
 * woken process. (Lecture 7-1, Part 8.)
 */
void
virtio_blk_intr(void) {
    spin_lock(&disk.lock);

    /* Acknowledge the interrupt at the device. Reading INTERRUPT_STATUS
     * tells us why it fired; writing those bits back to INTERRUPT_ACK
     * clears them. We ack both defined causes (a VRING completion is the
     * one we act on; CONFIG we don't use but still clear). */
    virtio_write(VIRTIO_MMIO_INTERRUPT_ACK, virtio_read(VIRTIO_MMIO_INTERRUPT_STATUS) &
                                                (VIRTIO_MMIO_INT_VRING | VIRTIO_MMIO_INT_CONFIG));

    /* Reap completions. Two counters chase each other:
     *   disk.used->idx — the DEVICE's producer counter. At init we gave
     *                    the device the used ring's base address, so it
     *                    owns the whole structure and writes it by DMA —
     *                    filling ring[] and bumping idx as it finishes
     *                    each chain. idx is just one field it owns.
     *   disk.used_idx  — OUR consumer cursor: how far we've reaped. Only
     *                    we touch it.
     * Every chain in [used_idx, used->idx) is newly complete.
     *
     * != not <: both are uint16 and the device's idx wraps past 65535;
     * a < test would stop reaping across the wrap. */
    __sync_synchronize();
    while (disk.used_idx != disk.used->idx) {
        int id = disk.used->ring[disk.used_idx % NUM_DESC].id;
        struct buf *b = disk.info[id];
        b->disk = 0;
        free_chain(id);
        disk.info[id] = NULL;
        disk.used_idx++;
    }
    wq_wake_all(&disk.wq);

    spin_unlock(&disk.lock);
}
