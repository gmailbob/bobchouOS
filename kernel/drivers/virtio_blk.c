/*
 * virtio_blk.c — VirtIO block device driver (virtio-mmio transport).
 *
 * Talks to QEMU's virtio-blk device at VIRTIO0. The driver owns one
 * virtqueue (NUM_DESC descriptors) guarded by a single spinlock, and
 * exposes a synchronous read/write built on an async submit primitive.
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
     *   free[i] == 1  → descriptor i is available (a simple boolean
     *                   array; alloc/free scan or stack it however you
     *                   like — see alloc_desc/free_desc below).
     *   info[i]       → for a descriptor that HEADS an in-flight chain,
     *                   the buf it belongs to, so the interrupt handler
     *                   can wake the right sleeper. */
    int free[NUM_DESC];
    struct buf *info[NUM_DESC];

    /* Synchronization */
    struct spinlock lock;
    struct wait_queue wq; /* bufs sleep here waiting for completion */
} disk;

/* ====================================================================
 * Descriptor allocation
 * ==================================================================== */

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
    if (!disk.free[i])
        panic("free_desc: attempt to double free disk.free[%d]", i);
    disk.free[i] = 1;
    /* optional: memset(&disk.desc[i], 0, sizeof(disk.desc[i])); */
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
        disk.free[i] = 1;
        if (disk.desc[i].flags & VIRTQ_DESC_F_NEXT)
            i = disk.desc[i].next;
        else
            break;
    }
}

/* ====================================================================
 * Device initialization (8-step handshake — Lecture 7-1, Part 3)
 * ==================================================================== */

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

    virtio_read(VIRTIO_MMIO_DEVICE_FEATURES);
    virtio_write(VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0); /* negotiate nothing */

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

    if (!(disk.desc = kalloc()) || !(disk.avail = kalloc()) || !(disk.used = kalloc()))
        panic("virtio_blk_init: failed to kalloc desc, avail or used");

    virtio_write(VIRTIO_MMIO_QUEUE_NUM, NUM_DESC);

    virtio_write(VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32)(uint64)disk.desc);
    virtio_write(VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32)((uint64)disk.desc >> 32));
    virtio_write(VIRTIO_MMIO_QUEUE_DRIVER_LOW, (uint32)(uint64)disk.avail);
    virtio_write(VIRTIO_MMIO_QUEUE_DRIVER_HIGH, (uint32)((uint64)disk.avail >> 32));
    virtio_write(VIRTIO_MMIO_QUEUE_DEVICE_LOW, (uint32)(uint64)disk.used);
    virtio_write(VIRTIO_MMIO_QUEUE_DEVICE_HIGH, (uint32)((uint64)disk.used >> 32));

    /* Step 8: tell the device we're live — set DRIVER_OK in STATUS. */
    status |= VIRTIO_STATUS_DRIVER_OK;
    virtio_write(VIRTIO_MMIO_STATUS, status);
    kprintf("virtio_blk: init done\n");
}

/* ====================================================================
 * Request submission (Lecture 7-1, Part 7)
 * ==================================================================== */

void
virtio_blk_submit(struct buf *b) {
    unsigned long irq;
    spin_lock_irqsave(&disk.lock, &irq);

    /* Allocate three descriptors (d0=header, d1=data, d2=status) */
    for (;;) {
        if (count_free() >= 3)
            break;
        wq_sleep(&disk.wq, &disk.lock);
    }
    int d0 = alloc_desc();
    int d1 = alloc_desc();
    int d2 = alloc_desc();

    /* Fill the header in the buf */
    int write = 0; /* Hardcode to read for now. Later buffer cache will thread the flag */
    b->req.type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    b->req.reserved = 0;
    b->req.sector = b->blockno * SECTORS_PER_BLOCK;

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

    /* Record the chain owner and mark the buf in-flight */
    disk.info[d0] = b;
    b->disk = 1;

    /* Publish to the avail ring, then bump idx */
    disk.avail->ring[disk.avail->idx % NUM_DESC] = d0;
    __sync_synchronize();
    disk.avail->idx += 1;
    __sync_synchronize();

    /* Ring the doorbell queue 0*/
    virtio_write(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    spin_unlock_irqrestore(&disk.lock, irq);
}

/* ====================================================================
 * Synchronous wrapper (Lecture 7-1, Part 9)
 * ==================================================================== */

void
virtio_blk_rw(struct buf *b) {
    virtio_blk_submit(b);

    unsigned long irq;
    spin_lock_irqsave(&disk.lock, &irq);
    while (b->disk == 1)
        wq_sleep(&disk.wq, &disk.lock);
    spin_unlock_irqrestore(&disk.lock, irq);
}

/* ====================================================================
 * Interrupt handler — top half (Lecture 7-1, Part 8)
 * ==================================================================== */

void
virtio_blk_intr(void) {
    spin_lock(&disk.lock);

    /* Acknowledge the interrupt at the device. Reading INTERRUPT_STATUS
     * tells us why it fired; writing those bits back to INTERRUPT_ACK
     * clears them. & 0x3: mask both defined interrupt bits */
    virtio_write(VIRTIO_MMIO_INTERRUPT_ACK, virtio_read(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3);

    /* Reap completions: the device has advanced disk.used->idx past our
     * disk.used_idx for each chain it finished. */
    __sync_synchronize();
    while (disk.used_idx < disk.used->idx) {
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
