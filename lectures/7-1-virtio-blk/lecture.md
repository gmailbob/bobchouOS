# Lecture 7-1: VirtIO Block Driver

> **Where we are**
>
> Phase 6 built a complete user-mode environment: processes fork, exec
> ELF binaries, demand-page their heaps and stacks, and communicate
> with the kernel through system calls. Everything lives in RAM —
> programs are embedded in the kernel image via `.incbin`.
>
> But a real operating system stores programs and data on a
> **persistent storage device** — a disk. Starting with this round, we
> build a file system from the ground up, one layer at a time:
>
> ```
> Round 7-6:  open/read/write/close  (user-facing syscalls)
> Round 7-5:  directories + paths    (naming)
> Round 7-4:  inodes + bitmap        (on-disk structure)
> Round 7-3:  write-ahead log        (crash safety)
> Round 7-2:  buffer cache           (in-memory cache of blocks)
> Round 7-1:  virtio-blk driver      (talk to the hardware)  ← you are here
> ```
>
> This round focuses on the bottom layer: making the disk device move
> bytes between its platters (or flash cells) and our kernel's memory.
> Everything above — caches, logs, inodes, files — is just software
> layered on top of this single primitive: **read or write a block.**
>
> **What you will understand after this lecture:**
>
> - Why paravirtualization (virtio) exists and how it differs from hardware emulation
> - The MMIO-based device discovery and initialization handshake
> - Virtqueue anatomy: descriptor table, available ring, used ring
> - The three-descriptor chain that forms a block I/O request
> - DMA constraints and physical address requirements
> - How the driver submits requests and the device signals completion
> - Interrupt-driven I/O: top-half (ISR) and bottom-half (sleeping process)
> - The submit/complete split that allows pipelining multiple requests

> **xv6 book coverage:**
> Ch 9 §9.1–9.3 ("I/O Devices"), "virtio" section. Our approach
> differs: we embed request metadata in `struct buf` (Linux bio-style
> instead of xv6's side-array), use BSIZE=4096 (matching page size for
> future mmap), and expose an async submit + sync wrapper (instead of
> xv6's purely synchronous API).

---

## Part 1: How Operating Systems Talk to Storage

### The big picture: one pattern, many devices

Before we dive into virtio specifically, let's understand where a
block driver sits in a real system and why modern hardware and virtual
devices ended up looking nearly identical.

A production machine may have multiple storage backends active
simultaneously — an NVMe SSD for local files, a network-attached
volume, a USB drive, maybe a virtual disk. The OS handles all of
them through the same layered architecture:

```
                     Userspace
                         │ read() / write()
                         ▼
┌──────────────────────────────────────────────────┐
│          VFS (Virtual File System)               │
│  One API for all filesystems. Doesn't care       │
│  what's underneath.                              │
└───────┬──────────────────┬──────────────┬────────┘
        │                  │              │
   ext4 / XFS         APFS / ext4      NFS client
        │                  │              │
   block layer        block layer    TCP/IP stack
   (blk-mq)           (blk-mq)            │
        │                  │           NIC driver
   nvme.ko           virtio_blk.ko        │
        │                  │           network
   NVMe SSD           hypervisor
   (hardware)         (software)
```

**VFS** is the magic layer — it gives userspace one unified API
(`open`/`read`/`write`) regardless of whether the file lives on NVMe,
virtio, USB, or a remote server. Each filesystem driver translates
VFS operations into block I/O. Each block driver translates block I/O
into device-specific commands. We build the bottom box in this round.

### The emulation problem

So why virtio instead of having QEMU emulate a real NVMe controller?

QEMU *can* emulate real hardware — an Intel AHCI SATA controller,
for example. The guest OS uses the same driver it would use on bare
metal. But emulation is expensive: the guest writes a register, QEMU
traps the MMIO access, interprets what a real AHCI controller would
do (state machines, DMA engines, FIS parsing), updates internal state,
fires a virtual interrupt. Enormous work to maintain an illusion the
guest doesn't actually need.

### Paravirtualization: drop the illusion

**Paravirtualization** means the guest *knows* it's in a VM and
cooperates with the hypervisor through an optimized interface. Instead
of pretending to be an AHCI controller, QEMU says: "I'm a virtual
host. Here's a shared-memory ring buffer. Put your requests there,
ring the doorbell, I'll process them directly."

No emulated DMA engines. No register-level state machines. Just
efficient data transfer through shared memory. That ring buffer is
the heart of this round — but first, some context on where virtio
lives in the wider world.

### Side-by-side: EC2 (VM) vs. bare-metal Linux vs. macOS

Three real systems, same architecture, different bottom layers:

```
   EC2 instance              Bare-metal Linux           macOS (Apple Silicon)
   ─────────────             ────────────────           ────────────────────

   application               application               application
        ↓ read()                  ↓ read()                  ↓ read()
   Linux kernel (guest)      Linux kernel              XNU kernel
        ↓ VFS                     ↓ VFS                     ↓ VFS / UBC
   ext4 / XFS                ext4 / XFS                APFS
        ↓ page cache / bio        ↓ page cache / bio        ↓ block I/O
   blk-mq                    blk-mq                    IOKit
        ↓                         ↓                         ↓
   virtio-blk driver         nvme driver               IONVMeFamily
        ↓ virtqueue rings         ↓ SQ/CQ rings            ↓ SQ/CQ rings
   ─── VM exit ──────        ─── HW boundary ───       ─── HW boundary ───
   KVM / Nitro hypervisor    NVMe controller           Apple NVMe controller
        ↓                         ↓ FTL firmware            ↓ FTL firmware
   host NVMe / EBS network   NAND flash dies           NAND flash dies
```

Everything above the driver is **identical in structure** — VFS,
filesystem, page cache, block layer. The only layer that changes is
the driver and what sits below it. This is why our work in rounds
7-2 through 7-6 (buffer cache, log, filesystem, syscalls) will be
completely agnostic to the storage backend. They just call
`virtio_blk_rw(b)` and data appears.

If you ever ported bobchouOS to a physical RISC-V board with NVMe,
you'd replace `virtio_blk.c` with `nvme.c` — different register map,
different init handshake, same ring-buffer submit/complete pattern —
and nothing above would change.

### How an OS discovers which drivers to load

A real machine might have an NVMe SSD, a virtio disk, and a USB
drive plugged in at the same time. The OS discovers them at boot:

**PCI/PCIe bus enumeration (Linux on x86/ARM):**

```
Boot → scan PCI bus → read each device's (vendor_id, device_id)

  Slot 0: vendor=0x144d device=0xa809 → Samsung NVMe SSD
           → load nvme.ko

  Slot 1: vendor=0x1af4 device=0x1001 → virtio block device
           → load virtio_blk.ko

  USB:    vendor=0x0781 device=0x5583 → SanDisk flash drive
           → load usb_storage.ko
```

Each driver registers block devices with the kernel. The naming
convention reflects the device's internal structure:

| Driver family | Device name | Meaning |
|---------------|-------------|---------|
| NVMe | `/dev/nvme0n1p2` | Controller 0, namespace 1, partition 2. Namespaces let one physical drive present multiple independent "virtual drives" (used in enterprise for isolation). Consumer drives almost always have just `n1`. |
| Virtio | `/dev/vda1` | First virtio disk (`v`=virtio, `d`=disk, `a`=first), partition 1. Flat enumeration — virtual disks have no internal hierarchy. |
| SCSI/SATA | `/dev/sda1` | First SCSI-style disk, partition 1. Even SATA drives use this via libata compatibility. |
| MMC/eMMC | `/dev/mmcblk0p1` | First MMC block device, partition 1. Common on embedded / phones. |
| IDE (ancient) | `/dev/hda1` | 2 channels × 2 devices max = hda through hdd. Mostly extinct. |

The filesystem layer mounts them independently. (For a worked example
of how a commercial OS organizes its on-disk topology, see the macOS
APFS aside near the end of this lecture.)

**For virtio-mmio (our case):** there's no PCI bus. QEMU places
virtio devices at hardcoded MMIO addresses. Discovery is simpler —
we read the MagicValue + DeviceID at each known address. Real
RISC-V boards use a **device tree** (`.dtb`) — a data structure
passed by firmware describing what devices exist at which addresses.
The OS parses it at boot instead of hardcoding. That's a stretch
goal (real hardware port).

For bobchouOS: one device (virtio-blk at `0x10001000`). Trivial
discovery, but the architecture is the same.

### Virtio in the real world

Virtio is VM-only — it only exists when there's a hypervisor in
between. But VMs are everywhere:

| Context | Storage interface | You're using it when... |
|---------|------------------|-------------------------|
| AWS / GCP / Azure | virtio-blk or virtio-scsi | Any cloud server (including the machine running this session) |
| Docker Desktop (macOS/Windows) | virtio inside the Linux VM | Docker on a Mac |
| WSL2 (Windows) | virtio-like (Hyper-V) | Windows Subsystem for Linux |
| Android Cuttlefish | virtio-blk | Google's cloud Android testing |

The driver you're writing in this round is essentially the same code
running on millions of cloud instances right now. The "disk" might
not even physically exist on the same machine — AWS EBS is a
distributed storage system accessed over the network, but to the
guest kernel it looks like a local virtio-blk device.

### NVMe and virtio: convergent evolution

Here's the surprising part: NVMe (designed ~2011) adopted the **same
ring-buffer model** as virtio (2008). Compare:

| Aspect | Virtio (2008) | NVMe (2011) |
|--------|---------------|-------------|
| Submit channel | Available ring (array of descriptor heads) | Submission Queue (array of 64-byte commands) |
| Complete channel | Used ring (array of id + length) | Completion Queue (array of 16-byte entries) |
| Notify mechanism | MMIO doorbell write | MMIO doorbell write |
| Multiple queues | Multi-queue support (we use 1) | Up to 64K queue pairs (typically 1 per CPU) |
| Queue count motivation | One per vCPU to avoid lock contention | One per CPU core for same reason |

NVMe is essentially "virtio done in hardware." The ring-buffer
producer/consumer pattern proved so effective in VMs that hardware
architects adopted it for real silicon. Before NVMe, AHCI/SATA used a
completely different model (command slots, FIS structures, port
multipliers) — much more complex, less parallelizable.

The architectures are:

```
virtio:  driver → [shared memory rings] → software (hypervisor)
NVMe:    driver → [shared memory rings] → hardware (controller)
                   ^^^^^^^^^^^^^^^^^^^^
                   same pattern, same idea
```

The driver code looks structurally similar. The difference is what's
on the other side consuming the rings — software (fast, but overhead
of VM exit) vs. silicon (faster, direct hardware response).

**Apple Silicon note (M1 through M4):** the NVMe controller is
integrated directly into the SoC — not a separate PCIe card on the
motherboard. The "PCIe" bus is internal fabric within the chip.

This integration has two concrete effects:

- **~5μs latency** (vs. ~10-15μs for a discrete PCIe NVMe SSD). The
  signal stays on the same silicon die — no PCIe physical layer
  overhead, no external chip boundary to cross.
- **Near-instant power gating.** A discrete PCIe device needs
  microseconds (tens of µs for deep L1 substates) for power state
  transitions via the PCIe ASPM L1 entry/exit handshake. On-die,
  Apple gates the controller's clock in nanoseconds, waking it
  just-in-time when an I/O request arrives. This is a major reason
  MacBooks achieve good battery life despite fast storage — the SSD
  logic draws near-zero power between I/O bursts.

But the software interface is still standard NVMe: submission queues,
completion queues, doorbells — the same ring-buffer pattern. The
driver doesn't know or care that the controller is on-die.

### The virtio specification

**VirtIO** (Virtual I/O) is the standard paravirtual device interface,
maintained by the OASIS standards body. It defines:

1. **Transport** — how the guest discovers and configures devices
   (MMIO, PCI, or channel I/O)
2. **Virtqueues** — the shared-memory ring buffers for communication
3. **Device types** — block, network, console, GPU, etc. — each with
   a device-specific protocol on top of the generic virtqueue

The three transports:

| Transport | How device is found | Typical use case |
|-----------|---|---|
| **virtio-pci** | Standard PCI/PCIe bus enumeration (vendor `0x1af4`) | The dominant transport — KVM/QEMU on x86, AWS, GCP. Device looks like a normal PCIe card. Supports hot-plug. |
| **virtio-mmio** | Hardcoded MMIO addresses (or device tree) | Embedded/simple platforms — QEMU `virt`, early ARM VMs. No PCI infrastructure needed but no hot-plug. |
| **virtio-ccw** | IBM channel I/O subsystem | IBM s390x mainframes only. Niche. |

We use **virtio-mmio** because QEMU's RISC-V `virt` machine doesn't
require a full PCIe bus — virtio devices are wired at fixed MMIO
addresses. Production cloud VMs almost always use virtio-pci, but
using it would mean writing a PCI subsystem first (bus enumeration,
config-space parsing, BAR decoding, interrupt routing) before we even
reach the interesting virtio protocol — hundreds of lines of bus
plumbing that teaches a hardware standard, not OS design. With MMIO
we skip straight to the ring-buffer protocol, which is identical
regardless of transport.

(We don't include PCI as a stretch goal either — if you ever port to
a physical RISC-V board with PCIe, the bus driver would come as part
of that effort. As a standalone exercise it's low insight-per-effort:
encyclopedic hardware knowledge rather than reusable OS principles.)

QEMU places the virtio-blk device at `0x10001000`.

### The contract

The guest and host agree on:
- A region of physical memory holding three structures (descriptor
  table, available ring, used ring)
- The guest writes request descriptors and notifies the device
- The device processes requests and notifies the guest via interrupt
- Neither side touches the other's current working region without
  proper memory ordering

This is a **producer-consumer** protocol over shared memory — one of
the most fundamental patterns in systems programming.

---

## Part 2: The Same Pattern at Every Layer

We just saw the ring pattern spread *horizontally* — virtio and NVMe,
software and silicon, converging on the same shared-memory rings. It
also stacks *vertically*: the same producer-consumer structure recurs
at three different levels of a modern Linux system.

```
┌─────────────────────────────────────────────────────────────────┐
│  Layer             Producer → Consumer     When introduced      │
├─────────────────────────────────────────────────────────────────┤
│  Hardware level    Guest kernel → Device    Virtio (2008)       │
│  (this round)     via virtqueue rings                           │
│                                                                 │
│  Kernel level      Filesystem → Block      Linux bio (2003)     │
│  (7-2 onward)     layer via submit_bio                          │
│                                                                 │
│  Userspace level   Application → Kernel    io_uring (2019)      │
│  (not us, but      via SQ/CQ rings                              │
│   same pattern)                                                 │
└─────────────────────────────────────────────────────────────────┘
```

**io_uring** — added to Linux in 2019 by Jens Axboe (same person
behind blk-mq, whose story we tell in Part 4) — gives userspace a submission
queue (SQ) and completion queue (CQ) in shared memory. The
application posts requests without syscalls; the kernel drains them
and posts completions. Zero syscalls, zero context switches in the
hot path.

io_uring's design echoes the virtio ring structure — the shared
ideas:
- Shared memory between producer and consumer
- Indices/flags for synchronization (no locks in the fast path)
- Batching: post many requests, one doorbell/notification
- Completions arrive asynchronously

We implement the hardware level in this round. In 7-2, our buffer
cache will use a simpler variant of the kernel-level pattern (single
`struct buf` submission + sleep). The io_uring level is mentioned for
completeness — it's the same architecture at the syscall boundary.

---

## Part 3: Device Discovery & Initialization

### MMIO register map

The virtio-mmio transport exposes a device as a contiguous block of
memory-mapped registers. On QEMU's `virt` machine, the block device
lives at `0x10001000`. The registers (offsets from base):

| Offset | Name | R/W | Description |
|--------|------|-----|-------------|
| 0x000 | MagicValue | R | Must be `0x74726976` ("virt" in little-endian) |
| 0x004 | Version | R | Device version (we expect 2 — "non-legacy") |
| 0x008 | DeviceID | R | Device type (2 = block device) |
| 0x00c | VendorID | R | `0x554d4551` for QEMU |
| 0x010 | DeviceFeatures | R | Feature bits offered by device |
| 0x014 | DeviceFeaturesSel | W | Select which 32-bit feature word to read |
| 0x020 | DriverFeatures | W | Feature bits accepted by driver |
| 0x024 | DriverFeaturesSel | W | Select which 32-bit feature word to write |
| 0x030 | QueueSel | W | Select which virtqueue to configure |
| 0x034 | QueueNumMax | R | Max descriptors this queue supports |
| 0x038 | QueueNum | W | Number of descriptors we want to use |
| 0x044 | QueueReady | W | Write 1 when queue is fully configured |
| 0x050 | QueueNotify | W | Write queue index to notify device |
| 0x060 | InterruptStatus | R | Bitmask of pending interrupts |
| 0x064 | InterruptACK | W | Write to acknowledge (clear) interrupts |
| 0x070 | Status | R/W | Device status register (init handshake) |
| 0x080 | QueueDescLow/High | W | Physical address of descriptor table |
| 0x090 | QueueDriverLow/High | W | Physical address of available ring |
| 0x0a0 | QueueDeviceLow/High | W | Physical address of used ring |

All accesses are 32-bit. Note the addresses written to the device
must be **physical** addresses, not virtual — the device DMAs
directly to/from RAM (see Part 6, "Physical addresses only").

One setup prerequisite is easy to forget: this register block at
`0x10001000` is MMIO, so it must be **mapped into the kernel page
table** before the driver touches it. Like the UART, CLINT, and PLIC,
we identity-map one page for it in `vm_create_kernel_pt`
(`kvm_map(VIRTIO0_BASE, VIRTIO0_BASE, PG_SIZE, PTE_R | PTE_W)`).
Skip this and the very first `virtio_read(MAGIC_VALUE)` takes a load
page fault — the device is "there" in hardware but invisible to a
kernel running with paging on.

### The 8-step initialization handshake

The virtio spec defines a strict protocol for bringing up a device:

```
Step 1:  Read MagicValue, Version, DeviceID — verify device exists
         and is the type we expect (block = 2).

Step 2:  Write 0 to Status (reset device).

Step 3:  Write STATUS_ACKNOWLEDGE (1) to Status — "I see you."

Step 4:  Write STATUS_DRIVER (2) to Status — "I know how to drive you."

Step 5:  Read DeviceFeatures. Decide which we accept.
         Write accepted features to DriverFeatures.

Step 6:  Write STATUS_FEATURES_OK (8) to Status.
         Re-read Status to confirm device accepted our feature set.
         (If FEATURES_OK bit is clear, device rejected — abort.)

Step 7:  Set up virtqueue(s):
         - Select queue (QueueSel = 0)
         - Read QueueNumMax
         - Allocate descriptor table, available ring, used ring
         - Write their physical addresses to the device
         - Write QueueReady = 1   ← activates the queue

Step 8:  Write STATUS_DRIVER_OK (4) to Status — "I'm ready, go."
```

After step 8, the device is live. We can submit requests.

The last two writes — `QueueReady = 1` then `DRIVER_OK` — are the
"go-live switches," and the order matters: ready the queue *before*
announcing the driver is ready. Omitting either fails **silently** —
no error, the device just ignores everything you submit. So if a
freshly-written driver submits a request and `used->idx` never
advances, a missing one of these two is the first thing to check.

### Feature negotiation

The device offers a set of feature bits; the driver selects which it
supports. For a block device, important features include:

- `VIRTIO_BLK_F_SIZE_MAX` — max segment size
- `VIRTIO_BLK_F_SEG_MAX` — max segments per request
- `VIRTIO_BLK_F_RO` — device is read-only

**The select/data register pair.** The feature space is wider than 32
bits (64+ bits and growing), but the MMIO layout exposes only a 32-bit
`DeviceFeatures` / `DriverFeatures` register. To reach the whole space,
virtio banks it: a *selector* register picks which 32-bit word you're
talking about, and the *data* register reads or writes that word.

```
DeviceFeaturesSel = 0  → DeviceFeatures now reads feature bits 0..31
DeviceFeaturesSel = 1  → DeviceFeatures now reads feature bits 32..63

DriverFeaturesSel = 0  → DriverFeatures now writes feature bits 0..31
DriverFeaturesSel = 1  → DriverFeatures now writes feature bits 32..63
```

So accepting features is a two-write operation per word: set the
selector (which window), then write the data (which bits). Setting the
selector alone changes nothing — you still have to write the data
register to actually record your accepted bits.

We accept none of the optional features, so we write `0` to the data
register. But "none" still spans both words: bits 0..31 *and* 32..63.
A correct "accept nothing" therefore zeroes **both** banks —
`(Sel=0, Features=0)` and `(Sel=1, Features=0)` — not just word 0.
(QEMU happens to tolerate zeroing only word 0, since unwritten bits
default to not-accepted, but the spec-correct driver does both.)

This keeps the driver minimal. A production driver would read what the
device offers in each word, AND it with what the driver supports, and
write back the negotiated subset.

---

## Part 4: Virtqueue Anatomy

A virtqueue consists of three structures laid out in contiguous
physical memory:

```
┌─────────────────────────────────────────────────────────────┐
│                   DESCRIPTOR TABLE                          │
│  ┌──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┐  │
│  │  d0  │  d1  │  d2  │  d3  │  d4  │  d5  │  d6  │  d7  │  │
│  └──────┴──────┴──────┴──────┴──────┴──────┴──────┴──────┘  │
├─────────────────────────────────────────────────────────────┤
│                    AVAILABLE RING                           │
│  ┌──────────┬────────────────────────────────────────────┐  │
│  │ flags|idx│  ring[0] ring[1] ring[2] ... ring[N-1]     │  │
│  └──────────┴────────────────────────────────────────────┘  │
│  (written by driver, read by device)                        │
├─────────────────────────────────────────────────────────────┤
│                      USED RING                              │
│  ┌──────────┬────────────────────────────────────────────┐  │
│  │ flags|idx│  ring[0] ring[1] ring[2] ... ring[N-1]     │  │
│  └──────────┴────────────────────────────────────────────┘  │
│  (written by device, read by driver)                        │
└─────────────────────────────────────────────────────────────┘
```

### Descriptor table

A **descriptor** is a small struct that says: "there is a buffer at
this physical address, this many bytes long, with these permissions."
It's the fundamental unit of communication — every piece of data
exchanged between driver and device is described by a descriptor.
Descriptors can be **chained** via a `next` field: one logical I/O
request (e.g. read block 42) is expressed as a chain of multiple
descriptors (header → data → status), linked together.

The descriptor table is an array of `N` descriptors (we'll use N=8,
matching xv6). This is just a `#define NUM_DESC 8` — changing it to
128 or 256 requires zero code changes (the free-list, rings, and
arrays all size themselves off this constant). Two constraints on the
value: `NUM_DESC ≤ QueueNumMax` (what the device reports), and it must
be a **power of two** — the rings index with `idx % NUM_DESC` where
`idx` is a free-running `uint16` that wraps at 65536, so the modulo
only stays consistent across that wrap when NUM_DESC divides 65536.
QEMU's virtio-blk defaults to a queue size of 256 (hard max 1024).

Real-world values: Linux virtio-blk defaults to 256, NVMe drives
typically use 1023 per queue (the spec allows up to 65,536 entries
per queue). Production systems need deep queues because hundreds of
threads may issue I/O concurrently. For us, a single-lock buffer
cache with ~30 bufs means at most a few requests in flight — 8 is
plenty.

Each descriptor is 16 bytes:

```c
struct virtq_desc {
    uint64 addr;    // physical address of the buffer
    uint32 len;     // length of the buffer in bytes
    uint16 flags;   // NEXT, WRITE, INDIRECT
    uint16 next;    // index of next descriptor in chain
};
```

Flags:
- `VIRTQ_DESC_F_NEXT` (1) — another descriptor follows (chaining)
- `VIRTQ_DESC_F_WRITE` (2) — device writes to this buffer (vs. reads)

The `next` field is what makes chaining work — when the NEXT flag is
set, it holds the index of the following descriptor (a block I/O
request chains three of them; see Part 5).

### Available ring (driver → device)

The driver uses this to tell the device: "here are ready descriptor
chains."

```c
struct virtq_avail {
    uint16 flags;            // usually 0
    uint16 idx;              // next slot driver will write to (monotonically increasing)
    uint16 ring[NUM_DESC];   // each entry is the HEAD descriptor index of a chain
};
```

To submit a request: write the chain's head descriptor index into
`ring[idx % N]`, then increment `idx`. The device sees the new idx
and processes entries from its last-seen position up to the new idx.

### Used ring (device → driver)

The device uses this to tell the driver: "I finished these."

```c
struct virtq_used_elem {
    uint32 id;     // head descriptor index of the completed chain
    uint32 len;    // total bytes written by device (for read requests)
};

struct virtq_used {
    uint16 flags;
    uint16 idx;                     // next slot device will write to
    struct virtq_used_elem ring[NUM_DESC];
};
```

The driver checks: "has `used.idx` advanced since I last looked?" If
yes, new completions are available in the used ring. We keep our own
cursor (`used_idx`) and reap entries until it catches up to the
device's `used.idx`. Compare them with `!=`, not `<`: `idx` is a free-
running `uint16` that wraps past 65535 back to 0, and a `<` test would
stop reaping across that wrap. The `% NUM_DESC` maps the monotonic
counter onto the actual ring slot.

### The split: who writes what

| Structure | Driver (guest) | Device (host) |
|-----------|---------------|---------------|
| Descriptor table | writes (fills descs) | reads (follows chains) |
| Available ring | writes (publishes heads) | reads (finds new work) |
| Used ring | reads (reaps completions) | writes (posts completions) |

This split is critical: neither side writes what the other writes.
No locks needed between guest and host — just memory ordering (fences)
to ensure visibility. (Part 10 has the full ownership map, extended to
the per-buf fields, as a reference for the I/O walkthrough.)

### How the device knows the layout

This is worth pausing on, because it's easy to assume the driver
"tells" the device where each field lives. It doesn't. The byte-level
layout is a **shared contract fixed by the virtio spec** — both sides
compile it in independently, the way two programs exchanging a network
packet both know the header format without negotiating it.

At runtime we communicate only three things, via MMIO registers:

| Communicated at runtime | Fixed by the spec (baked into both sides) |
|-------------------------|-------------------------------------------|
| base address of each structure (the `QueueDesc/Driver/Device` register pairs) | the field offsets *within* each structure |
| queue size (`QueueNum`) | which side writes which field |
| doorbell / interrupt (timing) | endianness (little-endian) |

So when we write the used ring's base address `X` to the device, that
single number is the whole handoff. From it, the device computes every
field offset using *its own copy* of the spec layout:

```
X+0   flags           (le16)
X+2   idx             (le16)
X+4   ring[0]:  id at X+4, len at X+8     (two le32)
X+12  ring[1]:  id at X+12, len at X+16
...   ring[i]  at  X + 4 + 8*i
```

Our `struct virtq_used` is simply the C transcription of that same
spec layout. The device (QEMU's `hw/virtio/`) has the identical layout
in its code. Neither transmits it — both were built from virtio spec
§2.7. If our struct disagreed by even one byte (a reordered field, an
unexpected pad), the device would read `idx` from the wrong offset and
the whole protocol would silently corrupt.

Two consequences fall out of this "shared static layout" model:

- **No accidental padding allowed.** We rely on the C compiler laying
  the struct out with no gaps, so `idx` really is at offset 2 and
  `ring[]` at offset 4. It works here because the fields are naturally
  aligned (`uint16`s at even offsets, the 8-byte `virtq_used_elem` at a
  4-aligned offset) — no padding sneaks in. A struct that *could* pad
  would need `__attribute__((packed))` to force the spec layout.
- **Queue size is the one layout parameter that varies**, so it can't
  be hardcoded in the spec — it's negotiated at runtime. The device
  advertises a maximum (`QueueNumMax`), the driver picks a size ≤ that
  and writes it back (`QueueNum`). After that both sides size `ring[]`
  to the same count and both wrap with `% QueueNum`. Our `NUM_DESC`
  must equal what we wrote to `QueueNum`, or the two sides' modulo
  arithmetic would address different slots.

> **Aside: the trailing event-suppression fields.** The spec puts one
> more `le16` after each ring's `ring[]` array — `used_event` in the
> avail ring, `avail_event` in the used ring. These belong to the
> optional `VIRTIO_F_EVENT_IDX` feature (interrupt/notification
> throttling for high-throughput devices): when negotiated, each side
> writes the index at which it wants its *next* interrupt/notification,
> letting the other side suppress the ones in between. We don't
> negotiate the feature, so these fields sit inert — but they still
> occupy a spec slot, and because that slot comes *after* `ring[]`, its
> offset depends on the queue size. (Our structs name them rather than
> calling them "unused," so the layout reads as the real spec layout.)

### Our choice: single virtqueue, single lock

We use one virtqueue with a single spinlock. The alternative —
multi-queue with per-CPU submission — is architecturally significant
but belongs to a later phase. Here's why.

**Brief history: Linux blk-mq**

From 2003 to 2013, Linux had a single `request_queue` per block
device, protected by one `queue_lock`. For spinning disks doing ~200
IOPS, the lock was never hot — the disk was always the bottleneck.

Around 2010, SSDs changed everything. Devices could suddenly do
500,000+ IOPS. The single queue lock became the bottleneck — profiles
showed 70%+ of CPU time spent in `queue_lock` contention on high-end
NVMe storage. The software was slower than the hardware.

In 2013, Jens Axboe introduced **blk-mq** (multi-queue block layer):
- **Software staging queues** — one per CPU, no cross-CPU locking
- **Hardware dispatch queues** — map to the device's actual hardware
  queues (NVMe can expose up to 64K queues)

The lesson: **when the device is no longer the bottleneck, software
coordination becomes the bottleneck.** The architecture must match the
parallelism of the hardware beneath it. This same story played out
in networking (single netif_rx → RSS/multi-queue NICs → per-CPU NAPI).

**Why single-queue is correct for us now:**

Multi-queue block I/O is one leg of a three-legged stool:
1. Per-CPU scheduler (Phase 9)
2. Sharded buffer cache (per-bucket locks, not one global lock)
3. Multi-queue driver

Without all three, the single-lock buffer cache still serializes
access before it reaches the driver. Multi-queue with a single-lock
cache buys nothing. The full concurrent sweep — where all three
layers go lock-free in parallel — is a stretch goal for after Phase 9,
when multi-hart is running and we can feel the contention firsthand.

---

## Part 5: Block Request Protocol

### What is DMA?

The block protocol below leans on one hardware concept, so we
introduce it first. **DMA** — Direct Memory Access — means a device
reads/writes RAM directly, without the CPU moving each byte.

Without DMA (PIO — Programmed I/O, dominant through the 1980s–90s;
still used today for small config reads like PCI register access):

```
CPU reads one byte from device register → stores to RAM
CPU reads one byte from device register → stores to RAM
... repeat 4096 times for one block ...
(CPU is 100% busy just copying bytes — can't do anything else)
```

With DMA (UDMA became standard by the late 90s; all modern storage
— SATA, NVMe, virtio — uses DMA for bulk data transfer):

```
CPU tells device: "here's a RAM address, put 4096 bytes there"
Device's DMA engine copies directly to RAM
CPU does other work (runs another process, handles other traps)
Device fires interrupt: "done"
```

The CPU just sets up the transfer (writes the descriptor) and walks
away. The device is an independent **bus master** — it can read/write
RAM on its own through the memory bus.

In our driver, the descriptor's `addr` field IS the DMA instruction:

```c
desc[1].addr = physical_addr(&b->data);  // "device, DMA here"
desc[1].len  = BSIZE;                     // "this many bytes"
desc[1].flags = VIRTQ_DESC_F_WRITE;       // "you write, I'll read later"
```

When we ring the doorbell, the device performs the DMA — copies 4096
bytes directly into `b->data` without any CPU instruction executing
for the transfer itself. (Part 6 covers the constraints this imposes:
physical addresses, alignment, ownership.)

> **DMA vs. shared memory IPC — easy to confuse**
>
> Both are "fast because they bypass the middleman," but in different
> contexts:
>
> | | DMA (device ↔ RAM) | Shared memory IPC (process ↔ process) |
> |--|--|--|
> | Who bypasses what? | Device bypasses the CPU | Processes bypass the kernel |
> | Purpose | Bulk data transfer to/from hardware | Fast inter-process communication |
> | Setup | Physical address in descriptor | `mmap` or `shmget` |
> | Synchronization | Interrupt | Semaphore / futex |
> | "Fast" because | CPU is free during transfer | No kernel copy (unlike pipe/socket) |
>
> Same principle (eliminate the copying middleman), different contexts.
> We'll encounter shared memory IPC in Phase 8-4 (`mmap MAP_SHARED`).

### The three-descriptor chain

Every block I/O operation — read or write — is expressed as a chain
of exactly three descriptors:

```
Descriptor 0 (header)    Descriptor 1 (data)      Descriptor 2 (status)
┌───────────────────┐    ┌───────────────────┐    ┌──────────────────┐
│ addr → req.type   │    │ addr → buf.data   │    │ addr → buf.status│
│ len  = 16 bytes   │───▶│ len  = 4096       │───▶│ len  = 1 byte    │
│ flags = NEXT      │    │ flags = NEXT      │    │ flags = WRITE    │
│ next = 1          │    │        [+WRITE    │    │ (no NEXT flag,   │
└───────────────────┘    │         if read]  │    │  chain ends here)│
                         │ next = 2          │    └──────────────────┘
                         └───────────────────┘
```

(The indices 0, 1, 2 here are illustrative. In practice the three
descriptors are whatever slots the free-list hands out — e.g. 3, 5, 6
— linked by their `next` fields, not by adjacency.)

**Descriptor 0 — request header:**

```c
struct virtio_blk_req {
    uint32 type;     // VIRTIO_BLK_T_IN (read) or VIRTIO_BLK_T_OUT (write)
    uint32 reserved;
    uint64 sector;   // which sector on disk (in 512-byte units!)
};
```

Note: `sector` is always in **512-byte units** regardless of our
block size. To read our 4096-byte block number `b`, we pass
`sector = b * (BSIZE / 512) = b * 8`.

The device **reads** this descriptor (driver → device direction).

**Descriptor 1 — data buffer:**

Points to the 4096-byte data region. Direction depends on operation:
- **Read:** device writes data here → flag = `VIRTQ_DESC_F_WRITE`
- **Write:** device reads data here → flag = 0 (no WRITE flag)

**Descriptor 2 — status byte:**

Device writes a single byte:
- `0` = success
- `1` = I/O error
- `2` = unsupported operation

Always has `VIRTQ_DESC_F_WRITE` flag (device writes to it).

### Why three descriptors instead of one big struct?

Why not put everything in a single contiguous struct and use one
descriptor?

```c
// Hypothetical: one big buffer for the entire request
struct one_big_request {
    uint32 type;           // device reads this
    uint32 reserved;       // device reads this
    uint64 sector;         // device reads this
    uint8  data[4096];     // device reads OR writes (depends on operation!)
    uint8  status;         // device writes this
};
```

The problem: **DMA direction is per-buffer, not per-byte.** Each
descriptor has exactly one direction flag (`VIRTQ_DESC_F_WRITE` or
not). There's no way to say "bytes 0–15 are device-read, bytes
16–4111 are device-write, byte 4112 is device-write" within a single
descriptor. The DMA hardware treats each buffer as one unit with one
direction.

So we split into three descriptors, each with its own direction:

```
desc 0 (header):  no WRITE flag  → device reads from this buffer
desc 1 (data):    WRITE flag     → device writes to this buffer (for reads)
                  no WRITE flag  → device reads from this buffer (for writes)
desc 2 (status):  WRITE flag     → device writes to this buffer
```

Chaining expresses the complete request as: "read 16 bytes from here
(header), then write 4096 bytes there (data), then write 1 byte
there (status)" — all in a single logical operation.

A secondary benefit: **scatter-gather**. Because each descriptor
points to an independent physical address, the data buffer doesn't
need to be contiguous with the header or status. Production drivers
exploit this to reference data at multiple non-contiguous physical
addresses in a single request (we don't need this, but the mechanism
is there).

### Our choice: request metadata embedded in `struct buf`

Where do the request header and status byte live? Two approaches:

**xv6's approach — side array:**
```c
static struct virtio_blk_req reqs[NUM_DESC];  // separate array
static struct { struct buf *b; } info[NUM_DESC];  // parallel tracking
```
Descriptor slot 3 always uses `reqs[3]`. Simple but creates parallel
arrays that must stay in sync.

**Our approach — embedded in buf (Linux bio-style):**
```c
struct buf {
    uint32  dev;
    uint32  blockno;
    uint8   data[BSIZE];
    struct virtio_blk_req req;  // lives here
    uint8   status;             // lives here
    int     disk;               // in-flight flag
};
```

The buf IS the I/O request — no parallel-array bookkeeping, zero
allocation on the I/O path. The `info[]` array just holds a
`struct buf *` pointer (for wakeup on completion). This is closer to
Linux's `struct bio` model where I/O metadata travels with the data.

In Round 7-2, we add buffer-cache fields (valid, refcnt, sleep-lock,
LRU link) — purely additive, no existing fields change. Same
incremental pattern as `struct proc` growing across Phase 5 and 6.

---

## Part 6: DMA Constraints & Block Sizing

DMA's power (Part 5) comes with rules. Because the device reaches into
RAM on its own, the buffers we hand it must satisfy constraints the
CPU would normally paper over.

### Physical addresses only

The descriptor's `addr` field must be a **physical address**. The
device performs DMA bypassing the CPU's MMU — it sees raw physical
RAM, not our kernel's virtual address space.

Since our kernel uses identity mapping (virtual = physical for kernel
addresses), this is straightforward: `addr = (uint64)&buf->data`
gives us the physical address directly. On a kernel with a non-trivial
virtual-to-physical mapping, you'd need an explicit translation step.

### Alignment

The device reads the descriptor table, available ring, and used ring
by physical address, with hardware alignment assumptions baked in. The
modern (split-virtqueue) spec requires per-structure alignment:

| Structure | Required alignment |
|-----------|-------------------|
| Descriptor table | 16 bytes |
| Available ring | 2 bytes |
| Used ring | 4 bytes |

The *legacy* layout was stricter: the three structures had to sit in
one contiguous, **page-aligned** block at fixed relative offsets. The
modern layout relaxed this to the per-structure alignments above and
let each structure live at an independently-programmed address (which
is why there are three separate `QueueDesc/Driver/Device` address
register pairs).

We satisfy all of this trivially by giving **each structure its own
page** via `kalloc()`. A page is 4096-aligned, which covers 16/2/4
with enormous margin — and also satisfies the legacy page-aligned
layout, so the code is correct regardless of which the device expects.

**Why `kalloc` (a whole page) and not `kmalloc` (a tight fit)?** The
structures are small — for NUM_DESC=8, roughly 128 / 22 / 70 bytes —
so a slab allocator could pack them into far less than three pages.
But this is the wrong place to economize:

- **Alignment is a hardware contract, not a nicety.** `kalloc` gives a
  page boundary by construction. Relying on "the slab allocator happens
  to return 16-byte-aligned slots" couples the driver to an allocator
  implementation detail — if `kmalloc` later added a per-slot header,
  the descriptor table could land misaligned and the device would
  silently misbehave. A page boundary can't drift.
- **It's a one-time boot cost.** Three pages (12 KB) allocated once,
  for the lifetime of the kernel — not per-I/O. On a 128 MB machine
  that's noise. Saving ~3.7 KB once isn't worth the coupling risk.

This is a general rule worth internalizing: **use page-granularity
allocation (`kalloc`) for DMA buffers and hardware-facing structures
where a device dictates alignment; use the slab allocator (`kmalloc`)
for internal kernel objects** (inodes, file structs, small bookkeeping)
where you just need "some bytes" and natural C alignment is enough.
The virtqueue structures are squarely in the first category — the
device touches them directly by physical address.

The data buffer in `struct buf` doesn't need special handling: BSIZE =
PG_SIZE and bufs come from page-aligned memory, so its alignment also
comes for free.

### Lifetime and ownership

Once a descriptor chain is published to the available ring and the
device is notified, the **device owns** the memory those descriptors
point to until it posts a completion in the used ring. The driver
must not read or write those buffers during this window.

This is why we embed the request header and status byte inside
`struct buf` (Part 5) — the buf exists for the entire duration of
the I/O. There's no risk of freeing the request struct while the
device is still DMAing.

### Sector size history

The `sector` field in `virtio_blk_req` counts in 512-byte units.
This reflects the historical evolution of storage sector sizes:

| Era | Device | Sector size |
|-----|--------|-------------|
| 1980s–2000s | Spinning HDDs | 512 bytes (universal standard) |
| ~2010 | "Advanced Format" HDDs | 4096 physical, 512 logical ("512e" emulation) |
| Early SSDs (SATA) | SATA SSDs | 512 bytes (inherited from HDD interface) |
| Modern NVMe SSDs | NVMe drives | 4096 native; some expose 512 + 4096, let the OS choose |

**The 512e era (~2010–2020)** was the awkward middle: drives
physically used 4K sectors but the firmware translated 512-byte
requests for backwards compatibility. This caused **write
amplification** — a 512-byte write forced the drive to
read-modify-write a full 4K sector internally. Real pain for
databases with small random writes.

The industry is converging on 4K as the native sector size. Our
choice of BSIZE=4096 aligns with this reality — one block = one page =
one physical sector on modern hardware. The virtio `sector` field
remains in 512-byte units for spec compatibility, so our conversion
is `sector = blockno * 8`.

### Our choice: BSIZE = 4096 (page-aligned blocks)

The block size is the most "lock-in" decision in Phase 7 — it
propagates into the buffer cache (7-2), log (7-3), bitmap math (7-4),
inode layout (7-4), and mkfs. Changing it later means rewriting
constants across 5 rounds.

We chose BSIZE = PG_SIZE = 4096. This means:
- One block = one page = one `struct buf` = one DMA transfer
- In Phase 8-4 (mmap), one file page maps to exactly one disk block
  — no stitching multiple blocks into one page
- Aligns with modern hardware (NVMe native sector = 4K)
- One indirect block holds 4096/4 = 1024 pointers → covers 4 MB per
  indirect (vs. xv6's 1024/4 = 256 pointers → 256 KB)

The cost: each buf uses 4 KB of data memory. For a toy filesystem on
a 16 MB disk image, we have ~4096 blocks total. The bitmap is tiny
(512 bytes). Acceptable.

---

## Part 7: Submitting Requests

### Free descriptor management

We have 8 descriptors. Some are in-flight (device is using them),
some are free. We track free descriptors with a simple free-list:

```c
static int free_desc[NUM_DESC];   // indices of free descriptors
static int nfree;                 // how many are free
```

`alloc_desc()` pops a free index; `free_desc_chain()` pushes a chain
of descriptors back. When `nfree < 3`, the caller must sleep — not
enough slots for a complete request chain.

### The submission sequence

To submit a read request for `struct buf *b`:

```
1. Allocate 3 descriptors (d0, d1, d2) from the free list.

2. Fill descriptor 0 — header:
   desc[d0].addr = physical_addr(&b->req)
   desc[d0].len  = sizeof(struct virtio_blk_req)
   desc[d0].flags = VIRTQ_DESC_F_NEXT
   desc[d0].next  = d1

3. Fill descriptor 1 — data:
   desc[d1].addr = physical_addr(&b->data)
   desc[d1].len  = BSIZE
   desc[d1].flags = VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE  (for reads)
   desc[d1].next  = d2

4. Fill descriptor 2 — status:
   desc[d2].addr = physical_addr(&b->status)
   desc[d2].len  = 1
   desc[d2].flags = VIRTQ_DESC_F_WRITE
   desc[d2].next  = 0  (unused — no NEXT flag)

5. Record which buf this chain belongs to:
   info[d0].b = b

6. Publish to available ring:
   avail.ring[avail.idx % NUM_DESC] = d0   (head of chain)
   memory_fence()                           (ensure desc writes are visible)
   avail.idx += 1

7. Notify device:
   write MMIO QueueNotify = 0  (queue index)
```

After step 7, the device sees the new available entry, follows the
descriptor chain, performs the DMA, writes the status byte, posts a
used-ring entry, and fires an interrupt.

### Memory fences

The `memory_fence()` between step 6's ring write and the idx update
is critical. Without it, the compiler or CPU could reorder the
stores: the device might see the new `idx` before the ring entry is
actually written, and follow a stale descriptor index.

On RISC-V, we use `fence w, w` (write-write ordering) to ensure all
prior stores are visible before subsequent stores. In our codebase,
this is the `__sync_synchronize()` builtin or an inline `fence`
instruction.

---

## Part 8: Interrupt Delivery & Handling

### How the interrupt reaches us: PLIC

When the device completes a request, it asserts an interrupt line.
But the CPU doesn't know "which device" — all external devices funnel
into a single cause code (`IRQ_S_EXT = 9`). The **PLIC** (Platform-Level
Interrupt Controller) is the hardware that demultiplexes:

```
Device fires IRQ line
      ↓
PLIC (checks: priority > 0? enabled? above threshold?)
      ↓
Sets pending bit for hart 0's S-mode context
      ↓
CPU sees scause = IRQ_S_EXT (9) — "some external device wants attention"
      ↓
trap handler calls external_interrupt()
      ↓
plic_claim() → returns source number (1 = virtio, 10 = UART, etc.)
      ↓
dispatch to driver: virtio_blk_intr()
      ↓
plic_complete(irq) → re-arms the source
```

This is our **first device interrupt** — back in Phase 2 we handled
only the timer (a CLINT/software interrupt, which doesn't go through
the PLIC). So a small PLIC driver arrives here, even though the
controller is platform infrastructure rather than the lesson's focus.

**PLIC register layout (from PLIC_BASE = 0x0C000000):**

The PLIC is a large MMIO region with four functional areas. Each hart
has two "contexts" (M-mode = even, S-mode = odd); hart 0's S-mode
context is **context 1**.

```
Offset          Region                          Stride
──────────────────────────────────────────────────────────────────
0x000000        Priority                        4 bytes per source
                source N at PLIC_BASE + 4*N
                value 0 = disabled, higher = more urgent

0x002000        Enable bitmaps                  0x80 (128 bytes) per context
                context 0 (M-mode) at +0x2000
                context 1 (S-mode) at +0x2080
                128 bytes × 8 = 1024 possible sources per context.
                Each bit = one source. Bit 1 = virtio, bit 10 = UART.

0x200000        Threshold + Claim/Complete      0x1000 (4K) per context
                context 0: threshold at +0x200000, claim/complete at +0x200004
                context 1: threshold at +0x201000, claim/complete at +0x201004
                threshold: mask sources at or below this priority
                claim/complete: same register — read to claim, write to complete
```

The 4K stride between contexts in the threshold/claim region is
deliberate waste for alignment — hardware likes page-aligned regions.

When you turn these into macros, compute each address as
`base + ctx * stride` from the **context-0 base** — e.g.
`PLIC_ENABLE(ctx) = PLIC_BASE + 0x2000 + ctx * 0x80`. The tempting
mistake is to hardcode context 1's address as the base
(`0x2080 + ctx * 0x80`), which double-counts the stride and silently
programs the *wrong* context. The enable bit then lands in a context
nothing reads, the interrupt is never delivered, and the caller sleeps
forever even though the device completed the I/O.

**Who assigns the source numbers?** The SoC/board designer — they're
physical wiring decisions, not software-configurable. Each device's
interrupt output is hardwired to a specific PLIC input pin. On QEMU's
`virt` machine:

```
IRQ 1   ← virtio-mmio slot 0 (0x10001000)
IRQ 2   ← virtio-mmio slot 1 (0x10002000)
...
IRQ 8   ← virtio-mmio slot 7 (0x10008000)
IRQ 10  ← 16550 UART (0x10000000)
```

You can't remap these in software — the pin is the pin. On real
hardware, the OS discovers these mappings from a **device tree**
(`.dtb`) passed by firmware. We hardcode `VIRTIO0_IRQ = 1` and
`UART0_IRQ = 10` because QEMU `virt` is a fixed platform.

QEMU's `virt` numbering puts hart 0's S-mode handler at PLIC "context
1" (context 0 is hart 0 M-mode). Phase 9 will compute the context
from the hartid; for now it's hardcoded.

**`plic_init()` programs four things for context 1:**

```
1. Priority[1] = 1          (source 1 is "on" — priority > 0)
2. Enable[ctx 1] |= (1<<1)  (source 1 enabled for this context)
3. Threshold[ctx 1] = 0     (accept any priority > 0)
4. sie |= SEIE              (S-mode external interrupts reach the CPU)
```

**Two mechanics worth calling out** (the code depends on them and
they're easy to get wrong):

- **Claim and complete use the *same* register.** A *read* of the
  claim/complete register claims the highest-priority pending source
  (and returns its number); a *write* of that number back to the same
  register signals completion. One register, two operations
  distinguished by read vs. write.
- **Completion is mandatory — it gates the source.** After a claim,
  the PLIC will not deliver another interrupt for that source until you
  complete it. Forget `plic_complete()` and the device goes silent
  forever (its completions pile up in the used ring, but the interrupt
  never fires again). This gating is exactly what makes the
  top/bottom-half cycle safe: the source stays masked while we handle
  it, so we can't re-enter for the same device.

### The interrupt path (device side)

When the device completes a request:
1. It writes a used-ring entry (`id` = head descriptor, `len` = bytes)
2. It increments `used.idx`
3. It asserts its interrupt line → PLIC routes as described above

### Top half (interrupt handler)

The interrupt handler runs with interrupts disabled. It must be fast:

```
1. Read InterruptStatus register — confirm it's a used-buffer notification.
2. Write InterruptACK to clear the interrupt.
3. Scan the used ring from our last-seen position to used.idx:
   - For each used entry:
     - Recover the buf: b = info[used_ring.ring[i].id].b
     - Mark it done: b->disk = 0
     - Free the descriptor chain back to the free list
     - Wake the sleeping process: wakeup(b)
4. Update our last-seen used index.
```

**The InterruptStatus / InterruptACK registers.** Reading
InterruptStatus tells you *why* the device interrupted; writing those
same bits back to InterruptACK clears them (the device won't lower its
interrupt line until you do). virtio-mmio defines only two cause bits:

| Bit | Name | Meaning |
|-----|------|---------|
| 0 (0x1) | VRING | a virtqueue has new used-ring entries (a request completed) |
| 1 (0x2) | CONFIG | device configuration changed (e.g. disk resized) |
| 2–31 | reserved | always 0 |

We only care about VRING (a completed I/O); we don't use config-change
notifications. The common idiom acks both defined bits with a `& 0x3`
mask — defensive, so we never write a reserved bit even if the read
returned something unexpected.

The handler does **no I/O processing** — it just marks bufs as done
and wakes sleepers. The actual "use the data" work happens in the
process context that was waiting.

### Bottom half (sleeping process)

The process that called `virtio_blk_rw(b)` is sleeping in a loop:

```c
while (b->disk == 1) {
    wq_sleep(&disk.wq, &disk.lock);
}
// b->data is now valid (for reads)
// b->status has the device's result
```

When wakeup fires, the process re-checks `b->disk`, finds it 0, and
proceeds. This is our standard sleep/wakeup pattern from Phase 5.

### Why this split matters

**Top half** (ISR context):
- Runs with interrupts disabled
- Must be fast — other interrupts are blocked
- Cannot sleep, cannot allocate, cannot do anything slow

**Bottom half** (process context):
- Runs as a normal schedulable thread
- Can sleep, allocate, do arbitrarily complex work

This separation is universal across operating systems. Even the
simplest embedded RTOS separates "acknowledge interrupt" from "process
the data." The principle: do the minimum in ISR context, defer
everything else to a thread.

---

## Part 9: The Driver API — Async Submit + Sync Wrapper

### Design decision: why not pure synchronous?

A purely synchronous driver would look like:

```c
void virtio_blk_rw(struct buf *b) {
    submit descriptors...
    notify device...
    while (b->disk) { /* spin or sleep */ }
}
```

This works but forces a one-at-a-time model. Consider two processes:

```
Process A:  bread(block 5)  → enter driver → submit → sleep
Process B:  bread(block 9)  → enter driver → submit → sleep
```

Both processes want different blocks. The device can handle both
concurrently — it has 8 descriptor slots. A purely synchronous design
that holds the lock during sleep would serialize them needlessly.

### Our approach: async primitive + sync wrapper

The **primitive** is the submit function — it posts the request and
returns immediately:

```c
// Post request to device. Does NOT wait for completion.
// Caller must sleep on b until interrupt handler sets b->disk = 0.
void virtio_blk_submit(struct buf *b) {
    spin_lock_irqsave(&disk.lock, &irq);
    // ... allocate descriptors, fill chain, publish, notify ...
    b->disk = 1;
    spin_unlock_irqrestore(&disk.lock, irq);
}
```

The **sync wrapper** is what the buffer cache calls:

```c
void virtio_blk_rw(struct buf *b) {
    virtio_blk_submit(b);
    spin_lock_irqsave(&disk.lock, &irq);
    while (b->disk)
        wq_sleep(&disk.wq, &disk.lock);
    spin_unlock_irqrestore(&disk.lock, irq);
}
```

This allows multiple bufs to be in-flight simultaneously: process A
submits and sleeps, process B submits and sleeps, both wake on their
respective interrupts.

### Locking against the interrupt handler

The lock calls above hide an important detail: `disk.lock` is shared
between **two kinds of context** — process context (submit, rw) and
interrupt context (the completion handler). That changes how the
process side must take the lock.

Consider the deadlock if the process side acquired the lock with
interrupts still enabled:

```
process: lock(disk.lock)              ← lock held, interrupts ON
process: ... ring the doorbell ...
device:  completes, raises IRQ
  ⚡ interrupt fires on THIS hart, mid-critical-section
handler: lock(disk.lock)              ← spins: lock held by the code
                                         we just interrupted
   → the interrupted process can't release until the handler returns,
     the handler can't return until it gets the lock → deadlock
```

On a single hart, an interrupt handler runs *on top of* whatever it
interrupted; if that code holds the lock, the handler spins forever.
The fix: the process side must acquire the lock with **interrupts
disabled** (`spin_lock_irqsave` in our kernel — `intr_off` then
acquire). The device interrupt then stays *pending* at the PLIC until
we release and re-enable; only then does the handler run, finding the
lock free.

The interrupt handler itself uses the **plain** `spin_lock` — it
already runs with interrupts disabled (trap entry masked them), so it
has nothing to save or restore.

| Context | Lock call | Why |
|---------|-----------|-----|
| `virtio_blk_submit` / `virtio_blk_rw` (process) | `spin_lock_irqsave` | shares the lock with the ISR — must mask IRQs to avoid the self-deadlock above |
| `virtio_blk_intr` (interrupt) | plain `spin_lock` | already in IRQ-disabled context; nothing to save |

**Sleeping inside the critical section is still fine.** `wq_sleep`
internally releases `disk.lock` before it context-switches and
re-acquires it on wakeup, and the saved interrupt flags ride along on
the sleeper's stack — restored correctly at the final
`spin_unlock_irqrestore`. This is the same lock-then-sleep pattern as
`proc_wait` in Phase 5; the rule "a lock shared with an ISR is taken
with interrupts off" composes cleanly with sleep/wakeup.

### Why the buffer cache uses the sync wrapper

The buffer cache's contract is: "`bread(blockno)` returns a buf with
valid data." The caller dereferences `b->data` on the very next line:

```c
struct buf *b = bread(dev, inode_block);
struct dinode *ip = (struct dinode *)(b->data + offset);
// read inode fields immediately
```

There is no "check back later" — the caller needs data NOW. This is
inherently a blocking operation. But "blocking" here means the process
**sleeps** (yields the CPU to the scheduler), not spin-waits. The
system keeps running other processes while this one waits for the
interrupt — no CPU cycles are wasted. The sync wrapper expresses the
correct semantics for this layer.

**The lesson:** async at one layer doesn't mean async at every layer.
Each layer chooses the interface that matches **its** contract:
- The device is async (hardware does I/O in parallel with CPU)
- The driver is async (multiple descriptors in flight)
- The buffer cache is sync (callers need data immediately)

Each boundary is a conscious design choice, not a failure to be
"more async."

---

## Part 10: Putting It Together

### Ownership map: who writes what

Before the walkthrough, here's the reference to keep beside it. The
driver and device share several structures in memory, and the bugs hide
in *who is allowed to write which field*. Every structure below lives in
RAM the driver allocated, but "allocated it" is not the same as "writes
it" — once we hand the device a base address, the device writes parts of
it by DMA.

```
  DRIVER (guest, our code)                        DEVICE (QEMU / host)
  ────────────────────────                        ────────────────────
  desc table   ─ writes addr/len/flags/next ───▶  reads (follows chains)
  avail ring   ─ writes ring[], bumps idx ─────▶  reads (finds new work)
               ◀──────── reads ring[], idx ─────  writes ring[], bumps idx   used ring
  buf.req      ─ writes type/sector ───────────▶  reads (the request header)
  buf.data     ─ writes (on WRITE) ────────────▶  reads (on WRITE)
               ◀─────────── reads (on READ) ────  writes (on READ, by DMA)   buf.data
  buf.status   ◀──────────────── reads ─────────  writes (1 byte result)
```

As a table — **A** = allocates, **W** = writes, **R** = reads:

| Structure | Driver | Device | Notes |
|-----------|--------|--------|-------|
| descriptor table | A, W | R | driver fills each descriptor; device only follows them |
| available ring | A, W | R | driver publishes chain heads + bumps `avail->idx` |
| used ring | A, **R** | **W** | device fills `ring[]` + bumps `used->idx`; driver only reads |
| `buf.req` (header) | W | R | type + sector; device reads it |
| `buf.data` | W (write op) / R (read op) | R (write op) / W (read op) | direction flips per operation — the whole point of the WRITE descriptor flag |
| `buf.status` | R | W | device writes the 1-byte result |
| `disk.free[]`, `disk.info[]`, `disk.used_idx` | A, W, R | — | driver-private bookkeeping; the device never sees these |

Three things make this tractable to reason about:

1. **For any shared field, exactly one side writes it.** The avail ring
   is driver-write / device-read; the used ring is the mirror. This
   single-writer rule (Part 4) is what lets the rings work lock-free
   between guest and host.
2. **`buf.data` is the one field whose direction flips** — device-written
   on a read, driver-written on a write. The `VIRTQ_DESC_F_WRITE` flag on
   the data descriptor is how we tell the device which way to DMA.
3. **`disk.free[]` / `info[]` / `used_idx` are ours alone** — pure
   driver-side bookkeeping the device has no knowledge of. Don't confuse
   `disk.used_idx` (our private consumer cursor) with `used->idx` (the
   device's producer counter inside the shared used ring).

### Complete I/O walkthrough

**Write block 5 to disk:**

```
1.  Caller invokes virtio_blk_rw(b)          b->blockno=5, b->data filled
2.  Submit: acquire lock, allocate 3 descriptors from the free list
3.  Fill request header in buf               type=WRITE, sector=40 (block 5 × 8)
4.  Set up the 3-descriptor chain            header → data → status
                                             (data has no WRITE flag — device reads from us)
5.  Record owner, mark buf in-flight         disk.info[d0]=b, b->disk=1
6.  Publish to avail ring + fences + doorbell
7.  Release lock, caller sleeps              CPU runs other processes

       ─── device side ───
8.  Device reads the chain                   follows d0→d1→d2 via next pointers
9.  Device DMAs b->data from guest RAM       4096 bytes → disk image at sector 40
10. Device writes b->status = 0 (success)
11. Device posts completion to used ring
12. Device fires interrupt                   PLIC pin 1 asserted

       ─── back in kernel ───
13. Trap → PLIC claim → virtio_blk_intr      top-half handler entered
14. ACK device, reap used ring               find d0 → recover b → set b->disk=0
15. Free descriptor chain, wake all sleepers
16. PLIC complete                            re-arm for next interrupt
17. Caller wakes, sees b->disk==0            block 5 is now on disk
```

**Read block 5 back:**

```
1.  Caller invokes virtio_blk_rw(b)          b->blockno=5, b->data is empty
2.  Submit: allocate descriptors, fill header    type=READ, sector=40
3.  Set up 3-descriptor chain                same structure, but data descriptor has WRITE flag
                                             (device writes into our buffer this time)
4.  Publish, fence, doorbell, sleep          same protocol as write

       ─── device side ───
5.  Device reads the chain
6.  Device DMAs INTO b->data from disk       4096 bytes → guest RAM
7.  Post completion, fire interrupt

       ─── back in kernel ───
8.  Trap → reap → wake                       same interrupt path
9.  Caller wakes                             b->data now holds what we wrote earlier
```

**The only difference between write and read:** the `type` field
(IN vs. OUT) and whether the data descriptor carries the WRITE flag
(telling the device which direction to DMA). The protocol machinery —
chain, avail ring, interrupt, used ring, wake — is identical.

### Init sequence

In `main.c`, after memory and process init (both are MMIO devices, so
they come after paging is enabled):

```c
void main(void) {
    // ... kalloc, vm, kmalloc, proc init ...
    plic_init();          // enable S-mode external interrupts + virtio source
    virtio_blk_init();    // discover + configure the block device
    // ... start scheduler ...
}
```

`virtio_blk_init()` performs the 8-step handshake, allocates the
virtqueue structures, and initializes the free descriptor list.

### Smoke test: read block 0

Before we have a filesystem, we can verify the driver works by reading
block 0 (which will eventually hold the superblock). This is an
**integration test**, not a unit test — `virtio_blk_rw` sleeps until
the completion interrupt fires, so it needs a live scheduler and
interrupts enabled. Our unit tests run at boot *before* the scheduler;
this one runs in the integration tier, inside a kernel process (the
integration runner calls `kthread_start()`, which releases the
scheduler-held lock and enables interrupts before any test runs):

```c
void test_virtio_blk(void) {
    struct buf *b = kmalloc(sizeof(struct buf));
    TEST_ASSERT(b != 0, "kmalloc buf");
    memset(b, 0, sizeof(struct buf));
    b->blockno = 0;
    b->status  = 0xff;   // sentinel: a real transfer overwrites this

    virtio_blk_rw(b);

    TEST_ASSERT(b->status == VIRTIO_BLK_S_OK, "read block 0: status OK");
    kmfree(b);
}
```

The `0xff` sentinel matters: a stubbed-out driver never touches
`status`, so the test reports FAIL rather than a false PASS (a
zero-initialized status would look like success).

In Round 7-4, block 0 will contain a superblock with a magic number.
For now, we just verify the DMA round-trip completes with status OK.
This test is scaffolding — it goes away once the buffer cache (7-2)
provides a real read path.

### File layout (new/modified)

```
kernel/
├── drivers/
│   ├── virtio_blk.c      (NEW — init, submit, intr, rw)
│   ├── virtio_blk.h      (NEW — driver public API)
│   ├── plic.c            (NEW — PLIC driver: init, claim, complete)
│   └── plic.h            (NEW — PLIC API + IRQ source numbers)
├── include/
│   ├── virtio.h          (NEW — virtqueue structs, register offsets)
│   ├── buf.h             (NEW — struct buf, BSIZE)
│   └── mem_layout.h      (MODIFIED — add VIRTIO0_BASE)
├── test/integration/
│   └── test_virtio_blk.c (NEW — read-block-0 smoke test, runs post-scheduler)
├── trap.c                (MODIFIED — external_interrupt() in both trap paths)
└── main.c                (MODIFIED — call plic_init + virtio_blk_init at boot)

Makefile                  (MODIFIED — new objs; attach fs.img to QEMU via
                            -drive + -device virtio-blk-device on the
                            virtio-mmio bus)
```

The disk needs a backing file. QEMU attaches `fs.img` (a blank raw
image for now; Round 7-4's mkfs will format it) to the virtio-mmio
bus so the device has something to DMA against.

---

## Aside: macOS on Apple Silicon — how a commercial OS lays out storage

This is unrelated to our virtio work — included as a reference for how
a modern commercial OS organizes its on-disk topology, as a contrast
to the Linux device model in Part 1.

macOS uses `diskutil list` instead of `lsblk`:

```
/dev/disk0 (internal, physical):
   #:                       TYPE NAME                    SIZE       IDENTIFIER
   0:      GUID_partition_scheme                        *500.3 GB   disk0
   1:             Apple_APFS_ISC Container disk1         524.3 MB   disk0s1
   2:                 Apple_APFS Container disk3         494.4 GB   disk0s2
   3:        Apple_APFS_Recovery Container disk2         5.4 GB     disk0s3

/dev/disk3 (synthesized):
   #:                       TYPE NAME                    SIZE       IDENTIFIER
   0:      APFS Container Scheme -                      +494.4 GB   disk3
                                 Physical Store disk0s2
   1:                APFS Volume Macintosh HD            12.6 GB    disk3s1
   2:              APFS Snapshot com.apple.os.update-... 12.6 GB    disk3s1s1
   3:                APFS Volume Preboot                 9.1 GB     disk3s2
   4:                APFS Volume Recovery                1.3 GB     disk3s3
   5:                APFS Volume Data                    143.6 GB   disk3s5
   6:                APFS Volume VM                      20.5 KB    disk3s6
```

Key differences from Linux:

- **No EFI partition.** Apple Silicon uses iBoot (same as iPhone), not
  UEFI. The `Apple_APFS_ISC` (iBoot System Container) holds firmware
  and boot policies instead.
- **Synthesized disks.** APFS presents each container as a virtual
  disk (`disk3`). Multiple APFS volumes share one pool of free space
  (like LVM thin provisioning on Linux).
- **Naming:** `/dev/disk0s2` = disk 0, slice 2. macOS uses "slice"
  (BSD heritage) instead of "partition."
- **Separate recovery partition.** `disk0s3` is a standalone recovery
  OS — isolated so you can restore even if the main container is
  completely corrupted.
- APFS volume **identifiers are never reused** once assigned. disk3s4 was a
  volume that existed at some point and was deleted.

---

## Quick Reference

### struct buf (minimal, expanded in 7-2)

```c
struct buf {
    uint32  dev;                  // device number
    uint32  blockno;              // which block on disk
    uint8   data[BSIZE];          // block data (4096 bytes)
    struct virtio_blk_req req;    // request header (type + sector)
    uint8   status;               // device result: 0=ok, 1=err, 2=unsupported
    int     disk;                 // 1 = in-flight, 0 = complete
};
```

### struct virtio_blk_req

```c
struct virtio_blk_req {
    uint32 type;       // VIRTIO_BLK_T_IN (read=0) or VIRTIO_BLK_T_OUT (write=1)
    uint32 reserved;
    uint64 sector;     // disk sector in 512-byte units (blockno * 8 for BSIZE=4096)
};
```

### Virtqueue Structures

```c
struct virtq_desc {       // 16 bytes per descriptor
    uint64 addr;          // physical address of buffer
    uint32 len;           // buffer length
    uint16 flags;         // VIRTQ_DESC_F_NEXT (1), VIRTQ_DESC_F_WRITE (2)
    uint16 next;          // next descriptor index (if NEXT flag set)
};

struct virtq_avail {      // driver → device
    uint16 flags;
    uint16 idx;           // next slot driver will write (monotonically increasing)
    uint16 ring[NUM_DESC];
};

struct virtq_used_elem {
    uint32 id;            // head descriptor index of completed chain
    uint32 len;           // bytes written by device
};

struct virtq_used {       // device → driver
    uint16 flags;
    uint16 idx;           // next slot device will write
    struct virtq_used_elem ring[NUM_DESC];
};
```

### MMIO Registers (key offsets from VIRTIO0_BASE = 0x10001000)

| Offset | Name | R/W | Purpose |
|--------|------|-----|---------|
| 0x000 | MagicValue | R | `0x74726976` ("virt") |
| 0x008 | DeviceID | R | 2 = block device |
| 0x070 | Status | R/W | Init handshake state |
| 0x034 | QueueNumMax | R | Max descriptors supported |
| 0x038 | QueueNum | W | Descriptors we want |
| 0x044 | QueueReady | W | 1 = queue configured |
| 0x050 | QueueNotify | W | Doorbell (write queue index) |
| 0x060 | InterruptStatus | R | Pending interrupts |
| 0x064 | InterruptACK | W | Clear interrupt |
| 0x080–0x0a4 | QueueDesc/Driver/Device Low/High | W | Physical addrs of queue structures |

### Device Status Bits (init handshake)

Bit values are not sequential — they're written in handshake order,
which is *not* numeric order (FEATURES_OK=8 is set before DRIVER_OK=4):

| Order | Bit value | Name | Meaning |
|-------|-----------|------|---------|
| 1st | 1 | ACKNOWLEDGE | "I see you" |
| 2nd | 2 | DRIVER | "I know how to drive you" |
| 3rd | 8 | FEATURES_OK | "We agree on features" |
| 4th | 4 | DRIVER_OK | "I'm ready, go" |

### Constants

| Name | Value | Meaning |
|------|-------|---------|
| BSIZE | 4096 | Block size = page size |
| NUM_DESC | 8 | Descriptor slots (tunable; QEMU queue default 256, hard max 1024) |
| VIRTIO0_BASE | 0x10001000 | MMIO base address (mem_layout.h) |
| VIRTIO0_IRQ | 1 | PLIC interrupt source (plic.h) |
| VIRTIO_BLK_T_IN | 0 | Read operation |
| VIRTIO_BLK_T_OUT | 1 | Write operation |

### Driver API

| Function | Signature | Purpose |
|----------|-----------|---------|
| `virtio_blk_init` | `void virtio_blk_init(void)` | 8-step handshake, allocate queues |
| `virtio_blk_submit` | `void virtio_blk_submit(struct buf *b)` | Post request, return immediately |
| `virtio_blk_rw` | `void virtio_blk_rw(struct buf *b)` | Submit + sleep until complete |
| `virtio_blk_intr` | `void virtio_blk_intr(void)` | ISR: reap used ring, wakeup sleepers |

### Key Invariants

| Invariant | Enforced by |
|-----------|-------------|
| Descriptors in use belong to the device | `disk` flag; driver doesn't touch until interrupt clears it |
| Available ring entries are valid before idx advances | `fence w,w` between ring write and idx increment |
| At least 3 free descriptors before submitting | Sleep in alloc loop until `nfree >= 3` |
| Physical addresses in descriptors (not virtual) | Identity mapping; explicit conversion if mapping changes |
