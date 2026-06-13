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
 * All of this is protected by virtio_lock. */
static struct {
    /* Virtqueue structures (allocated in virtio_blk_init). */
    struct virtq_desc *desc;   /* descriptor table (NUM_DESC entries) */
    struct virtq_avail *avail; /* available ring (driver → device) */
    struct virtq_used *used;   /* used ring (device → driver) */

    /* Free-descriptor bookkeeping. free[i] == 1 means descriptor i is
     * available. (A simple boolean array; alloc/free scan or stack it
     * however you like — see alloc_desc/free_desc below.) */
    int free[NUM_DESC];

    /* The last used-ring index we have processed. The device's
     * used->idx runs ahead of this as it completes requests; the
     * interrupt handler consumes [used_idx, used->idx). */
    uint16 used_idx;

    /* For each descriptor that heads an in-flight chain, which buf it
     * belongs to (so the interrupt handler can wake the right sleeper).
     * Indexed by head descriptor number. */
    struct buf *info[NUM_DESC];

    struct spinlock lock;
    struct wait_queue wq; /* bufs sleep here waiting for completion */
} disk;

/* ====================================================================
 * Descriptor allocation
 * ==================================================================== */

/*
 * alloc_desc — claim one free descriptor.
 *
 * Returns its index, or -1 if none are free. Caller holds disk.lock.
 *
 * TODO: scan disk.free[] for a free descriptor, mark it used, return
 * its index. Return -1 if all NUM_DESC are taken.
 */
static int
alloc_desc(void) {
    /* TODO */
    return -1;
}

/*
 * free_desc — return one descriptor to the free list.
 *
 * Caller holds disk.lock.
 *
 * TODO: mark descriptor i free again. (Optionally clear desc[i] for
 * cleanliness.) A double-free is a bug worth a panic.
 */
static void
free_desc(int i) {
    /* TODO */
}

/*
 * free_chain — free a whole descriptor chain starting at head `i`.
 *
 * Walks the chain via the NEXT flag / next field, freeing each link.
 * Caller holds disk.lock.
 *
 * TODO: loop: free desc i; if it has VIRTQ_DESC_F_NEXT, advance to
 * desc[i].next and repeat; otherwise stop.
 */
static void
free_chain(int i) {
    /* TODO */
}

/* ====================================================================
 * Device initialization (8-step handshake — Lecture 7-1, Part 3)
 * ==================================================================== */

void
virtio_blk_init(void) {
    spin_init(&disk.lock, "virtio_blk");
    wq_init(&disk.wq, "virtio_blk");

    /* Step 1: verify the device is present and is a block device.
     * Read MAGIC_VALUE (== VIRTIO_MAGIC), VERSION (== VIRTIO_VERSION),
     * DEVICE_ID (== VIRTIO_DEV_BLOCK), VENDOR_ID. panic() if any are
     * wrong — there is no recovery from "the device isn't there."
     *
     * TODO: read and check the four identity registers. */

    /* Steps 2-6: the status handshake + feature negotiation.
     *   2. write 0 to STATUS                       (reset)
     *   3. set ACKNOWLEDGE bit
     *   4. set DRIVER bit
     *   5. read DEVICE_FEATURES, decide what to accept, write
     *      DRIVER_FEATURES. We negotiate nothing fancy — clearing the
     *      features we don't want (e.g. VIRTIO_BLK_F_RO) is enough; the
     *      simplest correct driver writes back 0.
     *   6. set FEATURES_OK, then re-read STATUS and confirm FEATURES_OK
     *      is still set (panic if the device cleared it — it rejected us)
     *
     * Build the status value incrementally: each step ORs in one more
     * bit and writes the whole value back to STATUS.
     *
     * TODO: perform steps 2-6. */

    /* Step 7: set up queue 0.
     *   - QUEUE_SEL = 0
     *   - read QUEUE_NUM_MAX; panic if it's 0 (queue unavailable) or
     *     < NUM_DESC (device can't give us the depth we want)
     *   - allocate the three structures in DMA-able memory and zero
     *     them. kalloc() returns a zeroed, page-aligned page; one page
     *     each is more than enough for NUM_DESC=8.
     *   - QUEUE_NUM = NUM_DESC
     *   - write the physical addresses (split into LOW/HIGH halves):
     *       DESC   ← disk.desc
     *       DRIVER ← disk.avail
     *       DEVICE ← disk.used
     *   - QUEUE_READY = 1
     *
     * Helpers you'll want: a 64-bit physical address splits as
     * (uint32)pa into LOW and (uint32)(pa >> 32) into HIGH.
     *
     * TODO: allocate the queue, mark all descriptors free, program the
     * queue registers. */

    /* Step 8: tell the device we're live — set DRIVER_OK in STATUS. */

    /* TODO: write the final STATUS with DRIVER_OK set. */

    kprintf("virtio_blk: init done\n");
}

/* ====================================================================
 * Request submission (Lecture 7-1, Part 7)
 * ==================================================================== */

void
virtio_blk_submit(struct buf *b) {
    spin_lock(&disk.lock);

    /* Build and publish the 3-descriptor request chain. Steps:
     *
     * 1. Allocate three descriptors (d0=header, d1=data, d2=status).
     *    If fewer than three are free, wq_sleep(&disk.wq, &disk.lock)
     *    and retry. All-or-nothing: never hold some while sleeping for
     *    the rest (Lecture 7-1, Part 7).
     *
     * 2. Fill the header in the buf, then descriptor d0 (device reads).
     *    The disk addresses in 512-byte sectors, so convert the logical
     *    block number: sector = b->blockno * SECTORS_PER_BLOCK.
     *      b->req.type     = read ? VIRTIO_BLK_T_IN : VIRTIO_BLK_T_OUT
     *      b->req.reserved = 0
     *      b->req.sector   = sector
     *      desc[d0]: addr=&b->req, len=sizeof(b->req),
     *                flags=VIRTQ_DESC_F_NEXT, next=d1
     *    (Addresses are physical — identity-mapped, so the pointer IS
     *    the physical address.)
     *
     * 3. Fill descriptor d1 (data). Direction depends on the op:
     *      read  → device writes b->data → flags include VIRTQ_DESC_F_WRITE
     *      write → device reads b->data  → no WRITE flag
     *      desc[d1]: addr=b->data, len=BSIZE,
     *                flags=VIRTQ_DESC_F_NEXT [| VIRTQ_DESC_F_WRITE], next=d2
     *
     * 4. Fill descriptor d2 (status, device writes one byte, end of chain):
     *      desc[d2]: addr=&b->status, len=1,
     *                flags=VIRTQ_DESC_F_WRITE, next=0
     *
     * 5. Record the chain owner and mark the buf in-flight:
     *      disk.info[d0] = b;
     *      b->disk = 1;
     *
     * 6. Publish to the avail ring, then bump idx. The fence between the
     *    two is REQUIRED — the device must not see the new idx before
     *    the ring slot it points to (Lecture 7-1, Part 7, "Memory fences"):
     *      disk.avail->ring[disk.avail->idx % NUM_DESC] = d0;
     *      __sync_synchronize();
     *      disk.avail->idx += 1;
     *      __sync_synchronize();
     *
     * 7. Ring the doorbell:
     *      virtio_write(VIRTIO_MMIO_QUEUE_NOTIFY, 0);
     *
     * For Round 7-1 the only caller reads (block 0 smoke test), but
     * thread the read/write direction through so writes work too — the
     * buffer cache (7-2) will need both.
     *
     * TODO: implement steps 1-7. */

    spin_unlock(&disk.lock);
}

/* ====================================================================
 * Synchronous wrapper (Lecture 7-1, Part 9)
 * ==================================================================== */

void
virtio_blk_rw(struct buf *b) {
    virtio_blk_submit(b);

    /* Sleep until the interrupt handler clears b->disk. Standard
     * condition-loop sleep: hold the lock, re-check the condition each
     * wakeup (wakeups can be spurious).
     *
     * TODO:
     *   spin_lock(&disk.lock);
     *   while (b->disk == 1)
     *       wq_sleep(&disk.wq, &disk.lock);
     *   spin_unlock(&disk.lock);
     *
     * On return, b->status holds the device result. A production driver
     * would surface an error if status != VIRTIO_BLK_S_OK; for now you
     * may panic on a failed transfer. */
}

/* ====================================================================
 * Interrupt handler — top half (Lecture 7-1, Part 8)
 * ==================================================================== */

void
virtio_blk_intr(void) {
    spin_lock(&disk.lock);

    /* Acknowledge the interrupt at the device. Reading INTERRUPT_STATUS
     * tells us why it fired; writing those bits back to INTERRUPT_ACK
     * clears them. Do this before processing so a completion arriving
     * mid-handler still latches a fresh interrupt. */
    /* TODO:
     *   uint32 status = virtio_read(VIRTIO_MMIO_INTERRUPT_STATUS);
     *   virtio_write(VIRTIO_MMIO_INTERRUPT_ACK, status & 0x3); */

    /* Reap completions: the device has advanced disk.used->idx past our
     * disk.used_idx for each chain it finished. For each new entry:
     *   - read the head descriptor id from
     *       disk.used->ring[disk.used_idx % NUM_DESC].id
     *   - recover the buf: b = disk.info[id]
     *   - mark it done: b->disk = 0
     *   - free the descriptor chain (free_chain(id))
     *   - advance disk.used_idx
     * After the loop, wake everyone waiting (wq_wake_all) — each sleeper
     * re-checks its own b->disk.
     *
     * A fence before reading used->idx ensures we see the device's
     * writes to the ring (it wrote the entries before bumping idx).
     *
     * TODO:
     *   __sync_synchronize();
     *   while (disk.used_idx != disk.used->idx) {
     *       int id = disk.used->ring[disk.used_idx % NUM_DESC].id;
     *       struct buf *b = disk.info[id];
     *       b->disk = 0;
     *       free_chain(id);
     *       disk.info[id] = NULL;
     *       disk.used_idx += 1;
     *   }
     *   wq_wake_all(&disk.wq);
     */

    spin_unlock(&disk.lock);
}
