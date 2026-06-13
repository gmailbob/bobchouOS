/*
 * virtio.h — VirtIO MMIO transport + block device definitions.
 *
 * Constants and shared-memory structures for the virtio-mmio block
 * device on QEMU's `virt` machine. This header is reference material
 * (no logic) — the driver lives in kernel/drivers/virtio_blk.c.
 *
 * See Lecture 7-1, Parts 3-5, and the Quick Reference at the end.
 */

#ifndef VIRTIO_H
#define VIRTIO_H

#include "types.h"

/* ====================================================================
 * MMIO register offsets (from the device base, e.g. VIRTIO0).
 * All accesses are 32-bit. Modern (non-legacy, Version 2) layout.
 * ==================================================================== */

// clang-format off
#define VIRTIO_MMIO_MAGIC_VALUE       0x000 /* R: 0x74726976 ("virt") */
#define VIRTIO_MMIO_VERSION           0x004 /* R: version; 2 = non-legacy */
#define VIRTIO_MMIO_DEVICE_ID         0x008 /* R: 2 = block device */
#define VIRTIO_MMIO_VENDOR_ID         0x00c /* R: 0x554d4551 ("QEMU") */
#define VIRTIO_MMIO_DEVICE_FEATURES   0x010 /* R: feature bits offered */
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014 /* W: select feature word */
#define VIRTIO_MMIO_DRIVER_FEATURES   0x020 /* W: feature bits accepted */
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024 /* W: select feature word */
#define VIRTIO_MMIO_QUEUE_SEL         0x030 /* W: select virtqueue */
#define VIRTIO_MMIO_QUEUE_NUM_MAX     0x034 /* R: max queue size */
#define VIRTIO_MMIO_QUEUE_NUM         0x038 /* W: queue size we use */
#define VIRTIO_MMIO_QUEUE_READY       0x044 /* W: 1 = queue is live */
#define VIRTIO_MMIO_QUEUE_NOTIFY      0x050 /* W: doorbell (queue index) */
#define VIRTIO_MMIO_INTERRUPT_STATUS  0x060 /* R: pending interrupt bits */
#define VIRTIO_MMIO_INTERRUPT_ACK     0x064 /* W: acknowledge interrupts */
#define VIRTIO_MMIO_STATUS            0x070 /* R/W: init handshake state */
#define VIRTIO_MMIO_QUEUE_DESC_LOW    0x080 /* W: desc table phys addr lo */
#define VIRTIO_MMIO_QUEUE_DESC_HIGH   0x084 /* W: desc table phys addr hi */
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW  0x090 /* W: avail ring phys addr lo */
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH 0x094 /* W: avail ring phys addr hi */
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW  0x0a0 /* W: used ring phys addr lo */
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH 0x0a4 /* W: used ring phys addr hi */

/* Expected identity values (sanity check at init). */
#define VIRTIO_MAGIC          0x74726976 /* "virt" little-endian */
#define VIRTIO_VERSION        2          /* non-legacy */
#define VIRTIO_DEV_BLOCK      2          /* block device */

/* ---- Device status bits (written to STATUS during the handshake) ----
 * Note: written in handshake order, which is NOT numeric order
 * (FEATURES_OK=8 is set before DRIVER_OK=4). See Lecture 7-1, Part 3. */
#define VIRTIO_STATUS_ACKNOWLEDGE 1  /* "I see you" */
#define VIRTIO_STATUS_DRIVER      2  /* "I know how to drive you" */
#define VIRTIO_STATUS_DRIVER_OK   4  /* "I'm ready, go" */
#define VIRTIO_STATUS_FEATURES_OK 8  /* "We agree on features" */

/* ---- Descriptor flags ---- */
#define VIRTQ_DESC_F_NEXT  1 /* buffer continues into desc.next */
#define VIRTQ_DESC_F_WRITE 2 /* device writes (vs. reads) this buffer */

/* ---- Block request types (virtio_blk_req.type) ---- */
#define VIRTIO_BLK_T_IN  0 /* read: device → memory */
#define VIRTIO_BLK_T_OUT 1 /* write: memory → device */

/* ---- Block request status byte values ---- */
#define VIRTIO_BLK_S_OK     0
#define VIRTIO_BLK_S_IOERR  1
#define VIRTIO_BLK_S_UNSUPP 2

/* Number of descriptors in our queue. A #define — bumping it needs no
 * code changes (only NUM_DESC <= QUEUE_NUM_MAX must hold; QEMU defaults
 * to 256). 8 is plenty for our single-lock buffer cache. */
#define NUM_DESC 8

/* Disk sectors are always 512 bytes in the virtio protocol, regardless
 * of BSIZE. To address BSIZE-byte block n: sector = n * SECTORS_PER_BLOCK. */
#define SECTOR_SIZE        512
#define SECTORS_PER_BLOCK  (BSIZE / SECTOR_SIZE) /* 4096/512 = 8 */
// clang-format on

/* ====================================================================
 * Virtqueue shared-memory structures (driver <-> device).
 * Layout is fixed by the virtio spec; the device reads/writes these
 * by physical address, so they must live in DMA-able memory.
 * ==================================================================== */

/* One descriptor: a buffer at a physical address, with a length, flags,
 * and an optional link to the next descriptor in a chain. */
struct virtq_desc {
    uint64 addr;  /* physical address of the buffer */
    uint32 len;   /* buffer length in bytes */
    uint16 flags; /* VIRTQ_DESC_F_NEXT, VIRTQ_DESC_F_WRITE */
    uint16 next;  /* next descriptor index (if F_NEXT set) */
};

/* Available ring: driver publishes heads of ready descriptor chains. */
struct virtq_avail {
    uint16 flags;          /* usually 0 */
    uint16 idx;            /* next slot the driver will fill (wraps) */
    uint16 ring[NUM_DESC]; /* each entry = head descriptor index */
    uint16 unused;         /* used_event (we don't use it) */
};

/* One completion entry in the used ring. */
struct virtq_used_elem {
    uint32 id;  /* head descriptor index of the completed chain */
    uint32 len; /* total bytes the device wrote */
};

/* Used ring: device publishes completed chains. */
struct virtq_used {
    uint16 flags;
    uint16 idx; /* next slot the device will fill (wraps) */
    struct virtq_used_elem ring[NUM_DESC];
    uint16 unused; /* avail_event (we don't use it) */
};

/* Block request header. The device reads type+sector; the data buffer
 * and status byte are separate descriptors (see Lecture 7-1, Part 5). */
struct virtio_blk_req {
    uint32 type;     /* VIRTIO_BLK_T_IN / VIRTIO_BLK_T_OUT */
    uint32 reserved; /* must be 0 */
    uint64 sector;   /* start sector, in 512-byte units */
};

#endif /* VIRTIO_H */
