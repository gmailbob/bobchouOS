/*
 * virtio_blk.h — VirtIO block device driver public interface.
 *
 * Four entry points: init (boot handshake), the async submit primitive,
 * the synchronous read/write wrapper the buffer cache will call, and the
 * interrupt handler (invoked from the trap path on a virtio IRQ).
 *
 * See Lecture 7-1, Part 9 for the async-submit + sync-wrapper design.
 */

#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

struct buf;

/* Discover and configure the device (8-step handshake, allocate the
 * virtqueue, init the free-descriptor list). Call once at boot, after
 * paging and the PLIC are up. */
void virtio_blk_init(void);

/* Post an I/O request for `b` to the device and return immediately.
 * b->disk is set to 1; the interrupt handler clears it on completion.
 * The caller must sleep until b->disk == 0 (see virtio_blk_rw). */
void virtio_blk_submit(struct buf *b);

/* Synchronous read/write: submit `b`, then sleep until the device
 * signals completion. b->data holds the block on return (for reads);
 * b->status holds the device result. This is what the buffer cache
 * (Round 7-2) calls. */
void virtio_blk_rw(struct buf *b);

/* Interrupt handler. Called from the trap path when the PLIC reports a
 * virtio-blk interrupt: reaps the used ring, marks bufs done, wakes
 * their sleepers. */
void virtio_blk_intr(void);

#endif /* VIRTIO_BLK_H */
