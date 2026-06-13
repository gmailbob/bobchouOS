/*
 * test_virtio_blk.c — Integration test for the virtio-blk driver.
 *
 * Reads block 0 via virtio_blk_rw and checks the DMA round-trip
 * completes with status OK. Requires the scheduler (the driver sleeps
 * waiting for the interrupt completion).
 *
 * Temporary scaffolding for Round 7-1 — remove once the buffer cache
 * (7-2) provides a real read path.
 */

#include "test/test.h"
#include "buf.h"
#include "drivers/virtio_blk.h"
#include "kmalloc.h"
#include "string.h"

void
test_virtio_blk(void) {
    kprintf("[test_virtio_blk]\n");

    struct buf *b = kmalloc(sizeof(struct buf));
    TEST_ASSERT(b != 0, "kmalloc buf");
    memset(b, 0, sizeof(struct buf));
    b->blockno = 0;
    b->status = 0xff;

    virtio_blk_rw(b);

    TEST_ASSERT(b->status == VIRTIO_BLK_S_OK, "read block 0: status OK");

    kmfree(b);
}
