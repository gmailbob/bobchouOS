# Lecture 1-2: The UART Driver — Talking to Hardware

> **Where we are**
>
> In Round 1, we built the boot sequence: `entry.S` parks extra harts, zeros
> BSS, sets up the stack, and jumps to `kmain()`. We proved it works by doing
> a raw write to address `0x10000000` — the UART — and seeing "hello from
> bobchouOS" appear in the terminal.
>
> But that raw write was a hack. We wrote a single byte to a magic address
> with no initialization, no checking whether the hardware was ready, and no
> understanding of what we were actually talking to. It worked only because
> QEMU's virtual UART is extremely forgiving.
>
> This lecture explains what the UART actually is, how the 16550 chip works
> at the register level, and how to write a proper driver for it. By the end,
> you will understand:
>
> - What serial communication is and why UART matters for OS development
> - How memory-mapped I/O lets the CPU talk to devices
> - The complete register map of the 16550 UART
> - How to initialize the UART hardware
> - The difference between polling and interrupt-driven I/O
> - How xv6's UART driver works and how ours will differ
>
> **xv6 book coverage:** This lecture absorbs content from Chapter 5
> (Interrupts and Device Drivers), sections 5.1–5.2 and 5.5.

---

## Part 1: What is UART?

### The problem: how does a CPU talk to the outside world?

A CPU by itself is useless. It can compute, but it has no way to show you
the result. It needs **peripherals** — devices that connect it to the
outside world: screens, keyboards, disks, network cards.

But how does the CPU communicate with these devices? They speak completely
different "languages." A keyboard sends scan codes when keys are pressed. A
disk controller expects block addresses and DMA descriptors. A network card
deals in packets. The CPU needs a **protocol** — an agreed-upon way to
exchange data with each device.

For the simplest kind of communication — sending and receiving streams of
characters — the answer is **UART**: Universal Asynchronous
Receiver/Transmitter.

### What UART does

UART is a hardware device that converts between **parallel data** (what the
CPU works with — 8 bits at once on a data bus) and **serial data** (one bit
at a time over a wire). Think of it as a translator:

```
        CPU side                    Wire side
   (parallel: 8 bits)          (serial: 1 bit at a time)

   ┌───────────┐
   │  d7 d6 d5 │
   │  d4 d3 d2 │  ──→  UART  ──→  ──d0──d1──d2──d3──d4──d5──d6──d7──→
   │  d1 d0    │           (transmit)
   └───────────┘

   ←──d7──d6──d5──d4──d3──d2──d1──d0──  ──→  UART  ──→  8 bits to CPU
                                          (receive)
```

On a real computer, the serial wire might connect to:
- A **terminal** (the classic use — a VT100 terminal over RS-232)
- Another **computer** (serial console for servers)
- A **modem** (dial-up internet, fax machines)
- An **embedded device** (Arduino, Raspberry Pi debug console)

On QEMU, there is no physical wire. QEMU's virtual UART connects directly
to your terminal emulator (the window where you ran `make run`). When our
kernel writes a byte to the UART, QEMU sends it to your screen. When you
type a key, QEMU puts it in the UART's receive buffer.

### Why UART matters for OS development

UART is the **first device** almost every OS developer gets working, for
several reasons:

1. **It's simple.** The interface is just a handful of registers. Compare
   that to a GPU (thousands of registers, complex command queues) or a disk
   controller (DMA, scatter-gather lists, completion queues).

2. **It gives you text output.** Once UART works, you can print debug
   messages. This is essential — without output, debugging is nearly
   impossible. Every other driver you write later will use `kprintf()` (which
   uses UART) to print debug information.

3. **It's universal.** Every architecture has some form of UART. ARM boards,
   x86 PCs (COM ports), MIPS routers — they all have UARTs. The specific
   chip varies, but the concept is the same.

4. **It requires no complex setup.** Unlike a GPU (which needs framebuffer
   allocation, mode setting, pixel format configuration), UART needs about
   10 register writes to initialize.

### The "Universal" in UART

The "Universal" means the UART hardware can be configured for different
serial communication parameters:

| Parameter | What it controls | Common value |
|-----------|-----------------|-------------|
| **Baud rate** | Speed (bits per second) | 9600, 115200 |
| **Data bits** | Bits per character | 7 or 8 |
| **Stop bits** | Idle time between characters | 1 or 2 |
| **Parity** | Error detection bit | None, Even, Odd |

These parameters must match on both sides of the wire. If your UART sends
at 115200 baud but the receiver expects 9600, you get garbage.

On QEMU, the baud rate doesn't actually matter — data transfers
instantly because there's no physical wire with timing constraints. But we
still configure it properly, because:
- It's good practice (you might run this on real hardware someday)
- It exercises the real initialization sequence
- xv6 does it, and we want to understand why

### The "Asynchronous" in UART

"Asynchronous" means there is **no shared clock signal** between sender and
receiver. To understand what that means, let's first look at synchronous
protocols.

#### Synchronous: sender provides the clock

In synchronous protocols (like SPI or I2C), the sender transmits a **clock
signal on a separate physical wire** alongside the data. The receiver only
looks at the data line at specific moments — when the clock transitions
(rises or falls). Between those edges, the data line can be mid-transition,
noisy, or garbage — the receiver ignores all of that.

```
Synchronous (e.g., SPI Mode 0):

  Sender changes data    Receiver reads data
  on falling edge        on rising edge
          │              │
  CLK:  ──┐  ┌──┐  ┌──┐  ┌──
          └──┘  └──┘  └──┘
  DATA: ──d0────d1────d2────
          ↑     ↑     ↑  ↑
    data changes         data is stable,
    on falling edges     sampled on rising edges
```

Which edge is used depends on the protocol mode. For example, SPI has four
modes:

| SPI Mode | Samples on | Clock idle state |
|----------|------------|-----------------|
| Mode 0 | Rising edge | Low |
| Mode 1 | Falling edge | Low |
| Mode 2 | Falling edge | High |
| Mode 3 | Rising edge | High |

The key point: the sender and receiver agree in advance on which edge to
use. The sender changes data on one edge, and the receiver reads on the
other — this gives the data line time to settle before being sampled.

Synchronous protocols need more wires because of this:

| Protocol | Wires | What they carry |
|----------|-------|-----------------|
| UART (async) | 2 | TX (data), RX (data) |
| SPI (sync) | 4 | SCLK (clock), MOSI (data out), MISO (data in), CS (chip select) |
| I2C (sync) | 2 | SCL (clock), SDA (data) |

#### Asynchronous: no clock wire, count from the start bit

UART has no clock wire. Instead, both sides agree on the **baud rate**
beforehand, and each side uses its own internal clock. The receiver
synchronizes on a **start bit** at the beginning of each character.

Here's how it works. The data line is **high** when idle (nothing being
sent). When the sender wants to transmit a byte, it pulls the line **low**
— that's the start bit. The receiver watches for this high-to-low
transition:

```
Idle (high)     Start bit (low)     Data bits...
────────────┐
            └───────────────────────────────
            ↑
            Receiver sees this falling edge
            and knows: "a character is starting"
```

Once the receiver detects the start bit, it doesn't need to distinguish
start/data/stop bits by their value. It simply **counts**. It knows the
format (agreed in advance — e.g., 8 data bits, 1 stop bit), so it samples
at baud rate intervals and assigns meaning by **position**:

```
         Idle  Start   d0    d1    d2    d3    d4    d5    d6    d7   Stop  Idle
       ───────┐ ┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┐ ┌───────
              └─┘     │     │     │     │     │     │     │     │     └─┘
              ↑   ↑   ↑     ↑     ↑     ↑     ↑     ↑     ↑     ↑     ↑
              │   1   2     3     4     5     6     7     8     9    10
              │
           "GO!" — start counting

  Sample 1  = start bit (discard — we already know it's 0)
  Sample 2  = d0    ← "the 2nd sample is always d0"
  Sample 3  = d1
  ...
  Sample 9  = d7
  Sample 10 = stop bit (should be high — if not, framing error)
```

The receiver doesn't look at the value of each bit to figure out *what*
it is. It uses position: "the 5th sample after the start edge is d3,
regardless of whether it's high or low."

**Why the stop bit exists:** The stop bit forces the line back to high
(idle). This guarantees that the *next* start bit — another high-to-low
transition — is always detectable. Without it, if the last data bit
happened to be 0, the line would stay low and the receiver couldn't tell
where the next character begins.

After the stop bit, the receiver goes back to idle mode — watching for
the next falling edge. The cycle repeats for each character.

#### The core difference

| | Clock source | Timing |
|---|---|---|
| Synchronous | Shared clock wire from sender | Receiver uses sender's clock edges — always perfectly aligned |
| Asynchronous | Receiver's own internal clock | Receiver re-synchronizes on each start bit — can drift slightly |

The "drift" is why baud rates must match. If the receiver's clock is off
from the sender's, the sample points gradually shift. By bit 7 or 8, the
sample might land on the wrong bit. With 16x oversampling (sampling the
line 16 times per bit period and picking the middle), the 16550 tolerates
small clock differences — but both sides still need to be reasonably close.

#### Trade-offs: synchronous vs asynchronous

| | Synchronous (SPI, I2C) | Asynchronous (UART) |
|---|---|---|
| **Speed** | Fast — clock can run at tens of MHz | Slow — typically maxes out around 1–5 Mbps |
| **Wires** | More (need a clock line) | Fewer (just TX and RX) |
| **Distance** | Short — clock signal degrades over long wires | Long — no clock to degrade; RS-232 works over 15 m+ |
| **Complexity** | Sender must generate clock; receiver must handle it | Both sides need matching clocks; start/stop framing overhead |
| **Reliability** | High — sampling is perfectly aligned to sender's clock | Lower — clock drift can cause bit errors on long frames |
| **Typical use** | Chip-to-chip on the same board (sensor, flash, ADC) | Board-to-board or board-to-PC (debug console, modem, GPS) |

The pattern: synchronous is faster and more reliable, but needs more wires
and only works at short distances. Asynchronous is slower, but needs fewer
wires and works over longer distances. UART won the "debug console" role
because you only need two wires (TX and RX) to connect a board to a
terminal — and for printing debug text, speed doesn't matter.

> **Historical note:** UART dates back to the 1960s. The first widely used
> UART chip was the Western Electric 1602 (1971). The chip we're
> programming — the 16550 — was made by National Semiconductor in 1987 and
> became the standard for PC serial ports. Its register interface is so
> well-established that virtually every modern UART (including QEMU's
> virtual one) is "16550-compatible."

---

## Part 2: Memory-Mapped I/O

### How the CPU reaches devices

In Lecture 1-1, we mentioned that the CPU's address space is divided
between RAM and devices. Let's understand this more deeply, because it's
fundamental to how drivers work.

When the CPU executes a load or store instruction, the address goes onto
the **system bus**. A hardware circuit called the **address decoder** looks
at the address and routes the request to the right destination — RAM, UART,
PLIC, etc. The CPU doesn't know or care whether it's talking to RAM or a
device. It just issues loads and stores to addresses.

We already saw the QEMU virt memory map in Lecture 1-1, so we won't repeat
it here. The key new detail is **how** the decoder works: it checks the
**high bits** of the address. Hardware designers deliberately place each
device at a power-of-two aligned boundary so the decoder can use simple
bit comparisons:

```
Address         Binary (top 8 bits)    Decoder logic
──────────────  ─────────────────────  ─────────────────────────
0x0C000000      0000 1100 ...          bits 31:26 = 000011? → PLIC
0x10000000      0001 0000 ...          bits 31:28 = 0001?   → UART
0x80000000      1000 0000 ...          bit 31 = 1?          → RAM
```

This is why device base addresses are always "round" numbers like
`0x10000000` instead of, say, `0x10000037`. Aligned boundaries let the
decoder check just a few top bits with simple AND gates — no complex
comparison needed.

Each device also gets a **power-of-two sized region**. The UART gets 8
bytes (offsets 0–7), the PLIC gets 4 MB (2^22 bytes). The decoder only
decides *which device* based on the high bits. The low bits are passed
through to the device itself, which uses them to select a specific
register:

```
Address 0x10000005 (reading UART's LSR register):

  0001 0000 0000 0000 0000 0000 0000 0101
  ├──────────────────────────────┤ ├────┤
  High bits → decoder says       Low 3 bits → UART uses
  "this goes to UART"            to select register 5 (LSR)
```

### MMIO vs Port I/O

There are two main approaches for CPU-to-device communication:

**Memory-Mapped I/O (MMIO)** — devices appear as addresses in the normal
memory map. The CPU uses regular load/store instructions to access them.
This is what RISC-V uses.

**Port I/O** — devices have a separate address space accessed by special
instructions (`in` and `out` on x86). The CPU has dedicated I/O pins and
instructions.

| Approach | Instructions | Address space | Used by |
|----------|-------------|---------------|---------|
| MMIO | Regular `ld`/`sd` | Shared with memory | RISC-V, ARM, most modern architectures |
| Port I/O | Special `in`/`out` | Separate I/O space | x86 (legacy) |

> **Is MMIO slower because of the address decoder?**
>
> No. The decoder is combinational logic (AND/OR gates) that resolves
> within the same clock cycle — it adds no meaningful overhead. If
> anything, MMIO can be *faster*: regular load/store instructions go
> through the CPU's optimized memory pipeline, while x86's `in`/`out`
> instructions are serializing (they force the CPU to drain its pipeline,
> stalling execution).
>
> Both are extremely slow compared to RAM, but not because of the
> decoder — device registers simply can't be cached. (Note: "device
> register" means a small memory cell inside the UART chip, not a CPU
> register like `a0` or `sp`. The word "register" is overloaded — CPU
> registers are ~0.3 ns, device registers are ~100+ ns because every
> access must cross the system bus.) A RAM read hits L1 cache in ~1 ns.
> An MMIO read must travel all the way to the physical device and back —
> easily 100+ ns. The decode overhead is negligible next to that
> round-trip.

RISC-V only uses MMIO. There are no special I/O instructions. This is
simpler — you don't need to learn a separate set of instructions for device
access. A device register looks exactly like a memory location to the code.

### The volatile keyword

There's one critical difference between accessing RAM and accessing device
registers: **device registers have side effects**.

When you read from RAM, you get back whatever was stored there. Reading it
again gives the same value. When you read from a UART receive register, you
**consume** a character — the next read gives a different character (or
nothing). The read itself changes the device state.

This matters because the C compiler assumes memory doesn't change on its
own. Consider this code:

```c
uint8 *uart = (uint8 *)0x10000000;

// Wait until a character is available
while (*uart == 0)
    ;
char c = *uart;
```

The compiler might optimize this to:

```c
uint8 val = *uart;        // Read once
while (val == 0)           // Loop forever using cached value
    ;                      // (compiler thinks *uart can't change)
char c = *uart;
```

The optimizer sees that nothing in the loop modifies `*uart`, so it "helps"
by reading it once and reusing the value. But `*uart` is a device register
— it **can** change at any time when the UART receives a character. The
optimized code is an infinite loop.

The fix is the `volatile` keyword:

```c
volatile uint8 *uart = (volatile uint8 *)0x10000000;
```

`volatile` tells the compiler: "Every access to this pointer must result in
an actual load or store instruction. Do not cache, reorder, or optimize
away any access." This is **mandatory** for all MMIO device registers.

> **When must you use volatile?**
>
> - Accessing MMIO device registers — **always**
> - Accessing memory shared between interrupt handlers and normal code —
>   **always** (the interrupt can fire at any time and modify the value)
> - Accessing memory shared between CPU cores — **depends** (atomics or
>   memory barriers are often more appropriate, but volatile can help)
> - Accessing normal variables — **never** (it prevents useful
>   optimizations)
>
> A common pattern in OS code:
>
> ```c
> #define REG(base, offset) (*(volatile uint8 *)((base) + (offset)))
> ```
>
> This creates a macro that gives you a volatile pointer to any register at
> a given base address + offset. You'll see this pattern (or something very
> like it) in our UART driver.

### QEMU's memory map (review)

From Lecture 1-1, here's where our devices live:

```
Address         Device              Width    Purpose
──────────────  ──────────────────  ───────  ──────────────────────────
0x02000000      CLINT               64 KB   Timer, software interrupts
0x0C000000      PLIC                 4 MB   External interrupt controller
0x10000000      UART0                 8 B   Serial port (our target!)
0x10001000      virtio[0]            4 KB   Virtual disk
0x80000000      RAM                128 MB   Main memory
```

Notice the UART only occupies **8 bytes** of address space. That's 8
registers, each one byte wide. That's the entire hardware interface we need
to understand. Compare that to the PLIC (4 MB of registers) or a modern
GPU (hundreds of megabytes of MMIO space).

---

## Part 3: The 16550 UART Hardware

### A brief history

The 16550 is the specific UART chip that QEMU emulates. Understanding its
history helps explain some of its quirks:

| Year | Chip | What changed |
|------|------|-------------|
| 1971 | WE 1602 | First single-chip UART |
| 1981 | 8250 | Used in the original IBM PC. No FIFO — the CPU had to read each byte before the next one arrived, or data was lost. |
| 1987 | 16450 | Pin-compatible with 8250. Still no FIFO. |
| 1987 | 16550 | Added 16-byte FIFOs for both transmit and receive. This was a big deal — the CPU could now handle bursts of data without losing characters. |
| 1991 | 16550A | Bug fix for the 16550's FIFO. This is what everyone actually uses. |
| Today | Virtual | QEMU, embedded SoCs, and virtualization platforms implement 16550-compatible registers. The physical chip is gone, but the interface lives on. Even the AWS Graviton ARM server we're developing on right now uses Linux's 8250/16550 serial driver for its `/dev/ttyS0` — memory-mapped, same register layout. |

The key insight: the 16550 register interface became a **de facto
standard**. Even though nobody uses the physical chip anymore, new hardware
implements the same registers for compatibility. When QEMU says it has a
"16550 UART," it means the software interface matches — the registers are
at the same offsets and behave the same way.

### Register overview

The 16550 has 8 register offsets (0 through 7), but some offsets serve
**different purposes depending on whether you read or write them**, and
whether certain configuration bits are set. This is called **register
aliasing** — the chip designers needed more than 8 registers but only had 8
address lines, so they reused offsets.

Here's the complete register map:

```
Offset  Read                    Write                   When DLAB=1
──────  ──────────────────────  ──────────────────────  ──────────────
0x00    RHR (Receive Holding)   THR (Transmit Holding)  DLL (Divisor
        Read incoming byte      Write outgoing byte     Latch Low)

0x01    IER (Interrupt Enable)  IER (Interrupt Enable)  DLM (Divisor
        Same for read/write     Same for read/write     Latch High)

0x02    ISR (Interrupt Status)  FCR (FIFO Control)      —
        Read interrupt cause    Configure FIFOs

0x03    LCR (Line Control)      LCR (Line Control)      —
        Same for read/write     Same for read/write

0x04    MCR (Modem Control)     MCR (Modem Control)     —
        Same for read/write     Same for read/write

0x05    LSR (Line Status)       — (factory test)        —
        TX/RX status bits

0x06    MSR (Modem Status)      — (not used)            —
        CTS/DSR/RI/DCD

0x07    SPR (Scratch Pad)       SPR (Scratch Pad)       —
        General purpose byte    General purpose byte
```

Don't panic — we only need a handful of these for our Phase 1 driver.
Let's go through the important ones.

### The DLAB bit — register banking

Before we look at individual registers, we need to understand DLAB
(Divisor Latch Access Bit). This is bit 7 of the LCR register (offset
0x03):

```
LCR register (offset 0x03):
  bit 7    bit 6    bits 5:3   bit 2    bits 1:0
┌────────┬────────┬──────────┬────────┬──────────┐
│  DLAB  │ Break  │ Parity   │ Stop   │ Word     │
│        │ Ctrl   │ Select   │ Bits   │ Length   │
└────────┴────────┴──────────┴────────┴──────────┘
```

When DLAB = 0 (normal mode):
- Offset 0x00 is the data register (RHR for read, THR for write)
- Offset 0x01 is the Interrupt Enable Register

When DLAB = 1 (baud rate programming mode):
- Offset 0x00 becomes DLL (Divisor Latch Low byte)
- Offset 0x01 becomes DLM (Divisor Latch High byte)

This is how the chip fits baud rate configuration into the same address
space. You temporarily set DLAB = 1, write the baud rate divisor, then set
DLAB = 0 to return to normal operation.

```
                    DLAB = 0 (normal)          DLAB = 1 (baud rate)
                    ┌──────────────────┐       ┌──────────────────┐
Offset 0x00 read  → │  RHR (receive)   │       │  DLL (divisor    │
                    │                  │       │   low byte)      │
Offset 0x00 write → │  THR (transmit)  │       │  DLL (divisor    │
                    │                  │       │   low byte)      │
                    ├──────────────────┤       ├──────────────────┤
Offset 0x01       → │  IER (interrupt  │       │  DLM (divisor    │
                    │   enable)        │       │   high byte)     │
                    └──────────────────┘       └──────────────────┘
```

> **Why not just use separate registers?**
>
> The 16550 was designed in the 1980s when chip area was expensive. Using
> 8 address lines (3 bits for the register offset) kept the chip small and
> pin count low. The DLAB trick lets the same 8 offsets serve 10+ logical
> registers. It's a clever hack that worked well enough to become the
> standard for 40+ years.

### Register details: the ones we need

Let's look at each register we'll actually use in our driver.

> **What about offsets 0x04, 0x06, and 0x07?**
>
> We skip three registers because they're irrelevant to us:
> - **MCR (0x04)** and **MSR (0x06)** — modem control/status signals
>   (RTS, CTS, DSR, etc.). These are RS-232 handshake lines for
>   hardware flow control between modems and computers. We have no
>   modem, and QEMU's virtual UART ignores them.
> - **SPR (0x07)** — a scratch pad register. One byte of free storage
>   that the UART hardware ignores completely. It was sometimes used
>   by BIOS code to detect whether a UART chip was present (write a
>   value, read it back, check if it matches). We don't need it.

#### THR — Transmit Holding Register (offset 0x00, write, DLAB=0)

This is where you write bytes to send. Write a byte here, and the UART
transmits it over the serial line.

```
THR (offset 0x00, write-only when DLAB=0):
  bits 7:0
┌────────────────────────────────────────────┐
│  Data byte to transmit (8 bits)            │
└────────────────────────────────────────────┘
```

**Important:** You must check that the transmitter is ready (THR is empty)
before writing. If you write while the previous byte is still being sent,
the new byte overwrites the old one and you lose data. We'll use the LSR
register to check readiness.

On QEMU, the transmitter is essentially always ready (virtual hardware is
infinitely fast), but checking is correct behavior and needed on real
hardware.

#### RHR — Receive Holding Register (offset 0x00, read, DLAB=0)

This is where incoming bytes appear. When someone types a character (or
the other end of the serial link sends data), it shows up here.

```
RHR (offset 0x00, read-only when DLAB=0):
  bits 7:0
┌────────────────────────────────────────────┐
│  Received data byte (8 bits)               │
└────────────────────────────────────────────┘
```

**Important:** Reading RHR consumes the byte — it's gone from the hardware
after you read it. If the FIFO has more bytes, the next one becomes
available in RHR. If the FIFO is empty, reading RHR returns undefined data.
Always check LSR first to see if data is available.

The 16550 has a **16-byte receive FIFO**. If bytes arrive faster than the
CPU reads them, they queue up (up to 16). If the FIFO overflows, incoming
bytes are lost — this is called an **overrun error**, and it sets a flag in
the LSR register.

```
               16-byte receive FIFO
               Oldest byte exits first (FIFO = First In, First Out)

Serial   ┌────┬────┬────┬────┬────┬────┬─ ··· ─┬────┬────┬────┐   CPU
line  →  │ 16 │ 15 │ 14 │ 13 │ 12 │ 11 │       │  3 │  2 │  1 │ → reads
         └────┴────┴────┴────┴────┴────┴─ ··· ─┴────┴────┴────┘   RHR
          ↑ newest                                      oldest ↑

If FIFO is full and another byte arrives → overrun error!
```

#### IER — Interrupt Enable Register (offset 0x01, DLAB=0)

Controls which events trigger a hardware interrupt:

```
IER (offset 0x01, read/write when DLAB=0):
  bits 7:4    bit 3     bit 2     bit 1     bit 0
┌───────────┬─────────┬─────────┬─────────┬─────────┐
│  Reserved │ Modem   │ Line    │ THR     │ Receive │
│  (zero)   │ Status  │ Status  │ Empty   │ Data    │
│           │ Int     │ Int     │ Int     │ Avail   │
└───────────┴─────────┴─────────┴─────────┴─────────┘
```

| Bit | Name | When set (=1) |
|-----|------|---------------|
| 0 | Receive Data Available | Interrupt when a byte is received |
| 1 | THR Empty | Interrupt when transmit register is empty (ready for next byte) |
| 2 | Receiver Line Status | Interrupt on error (overrun, parity, framing, break) |
| 3 | Modem Status | Interrupt on CTS/DSR/RI/DCD change |

For Phase 1, we'll set IER to **0x00** — all interrupts disabled. We
haven't set up trap handling yet (that's Phase 2), so even if the UART
fires an interrupt, the CPU wouldn't know what to do with it. We'll use
**polling** instead (checking registers manually in a loop).

In Phase 2, when we add trap handling, we'll enable bits 0 and 1 to get
interrupts on receive and transmit-ready events.

#### FCR — FIFO Control Register (offset 0x02, write-only)

Controls the FIFO buffers:

```
FCR (offset 0x02, write-only):
  bits 7:6    bits 5:4   bit 3     bit 2     bit 1     bit 0
┌───────────┬──────────┬─────────┬─────────┬─────────┬─────────┐
│  RX FIFO  │ Reserved │ DMA     │ TX FIFO │ RX FIFO │ FIFO    │
│  Trigger  │ (zero)   │ Mode    │ Reset   │ Reset   │ Enable  │
│  Level    │          │ Select  │         │         │         │
└───────────┴──────────┴─────────┴─────────┴─────────┴─────────┘
```

| Bits | Name | Purpose |
|------|------|---------|
| 0 | FIFO Enable | 1 = enable both FIFOs. Required for 16550 mode. |
| 1 | RX FIFO Reset | 1 = clear the receive FIFO. Self-clearing (resets to 0). |
| 2 | TX FIFO Reset | 1 = clear the transmit FIFO. Self-clearing. |
| 7:6 | RX Trigger Level | How full the RX FIFO must be before generating an interrupt: 00=1 byte, 01=4 bytes, 10=8 bytes, 11=14 bytes |

We'll write **0x07** to FCR: enable FIFOs (bit 0) + reset both FIFOs
(bits 1 and 2). The trigger level (bits 7:6) doesn't matter for Phase 1
since we're not using interrupts.

```
FCR = 0x07:
  0000 0111
        |||
        ||└─ FIFO Enable = 1
        |└── RX FIFO Reset = 1 (clears any stale data)
        └─── TX FIFO Reset = 1 (clears any stale data)
```

> **Why reset the FIFOs during init?**
>
> The UART might have leftover data from before our kernel started.
> Maybe QEMU put something there during startup, or maybe (on real
> hardware) the chip retained data from a previous boot. Resetting both
> FIFOs ensures we start clean.

#### LCR — Line Control Register (offset 0x03)

Configures the data format — word length, stop bits, and parity:

```
LCR (offset 0x03, read/write):
  bit 7    bit 6    bits 5:3    bit 2    bits 1:0
┌────────┬────────┬───────────┬────────┬──────────┐
│  DLAB  │ Break  │  Parity   │  Stop  │  Word    │
│        │ Ctrl   │  Select   │  Bits  │  Length  │
└────────┴────────┴───────────┴────────┴──────────┘
```

| Bits | Name | Values |
|------|------|--------|
| 1:0 | Word Length | 00=5 bits, 01=6 bits, 10=7 bits, **11=8 bits** |
| 2 | Stop Bits | 0=1 stop bit, 1=1.5 (for 5-bit) or 2 stop bits |
| 5:3 | Parity | 000=none, 001=odd, 011=even, 101=force 1, 111=force 0 |
| 6 | Break Control | 1=force TX line low (break signal) |
| 7 | DLAB | Divisor Latch Access Bit (see above) |

We'll use **8 data bits, 1 stop bit, no parity** — written as "8N1" in
serial communication shorthand. This is the most common configuration:

```
LCR = 0x03 (during normal operation):
  0000 0011
       ||
       |└─ Word Length bit 0 = 1 ─┐
       └── Word Length bit 1 = 1 ─┘→ 11 = 8 data bits
  Stop Bits = 0 → 1 stop bit
  Parity = 000 → None
  DLAB = 0 → normal register access
```

During initialization, we write LCR = 0x80 (set DLAB=1) to access the
baud rate registers, then write LCR = 0x03 (8N1, DLAB=0) to set the
data format and return to normal mode. Part 4 walks through this step
by step.

#### LSR — Line Status Register (offset 0x05, read-only)

This is the **most important register for polling**. It tells you whether
the transmitter is ready and whether received data is available:

```
LSR (offset 0x05, read-only):
  bit 7    bit 6    bit 5    bit 4    bit 3    bit 2    bit 1    bit 0
┌────────┬────────┬────────┬────────┬────────┬────────┬────────┬────────┐
│  RX    │  TX    │  THR   │  Break │ Framing│ Parity │Overrun │  Data  │
│  FIFO  │  Empty │  Empty │  Int   │ Error  │ Error  │ Error  │  Ready │
│  Error │        │        │        │        │        │        │        │
└────────┴────────┴────────┴────────┴────────┴────────┴────────┴────────┘
```

The two bits we care about:

| Bit | Name | Meaning |
|-----|------|---------|
| **0** | **Data Ready** | 1 = at least one byte is in the RX FIFO, ready to read from RHR |
| **5** | **THR Empty** | 1 = the Transmit Holding Register is empty, safe to write a new byte |

The polling pattern for transmitting:

```c
// Wait until transmitter is ready
while ((read LSR) & (1 << 5)) == 0)
    ;   // spin — THR is not empty yet
// Now safe to write
write THR = character;
```

The polling pattern for receiving:

```c
// Check if data is available
if ((read LSR) & (1 << 0)) != 0) {
    char c = read RHR;   // consume the byte
}
```

#### DLL and DLM — Divisor Latch (offsets 0x00 and 0x01, DLAB=1)

These two registers together form a 16-bit divisor that sets the baud
rate. The formula is:

```
              clock frequency
baud rate = ─────────────────────
                16 × divisor
```

Or rearranged:

```
            clock frequency
divisor = ─────────────────────
             16 × baud rate
```

The 16550 in QEMU (and most implementations) uses a **1.8432 MHz**
reference clock. On a real board, this comes from a quartz crystal
oscillator soldered near the UART chip — a tiny component chosen
specifically because 1843200 ÷ 16 = 115200 exactly, giving clean
integer divisors for all standard baud rates. On QEMU, it's just a
constant in the emulator's source code. For 38400 baud:

```
divisor = 1843200 / (16 × 38400) = 1843200 / 614400 = 3
```

So DLL = 3, DLM = 0 (the divisor is 3, which fits in the low byte).

| Baud rate | Divisor | DLL | DLM |
|-----------|---------|-----|-----|
| 115200 | 1 | 0x01 | 0x00 |
| 57600 | 2 | 0x02 | 0x00 |
| **38400** | **3** | **0x03** | **0x00** |
| 19200 | 6 | 0x06 | 0x00 |
| 9600 | 12 | 0x0C | 0x00 |

xv6 uses a divisor of 3 (38400 baud). We'll do the same.

> **Why 38400 baud, not 115200?**
>
> On QEMU it doesn't matter — the virtual UART transfers instantly
> regardless of the configured baud rate. xv6 uses 38400 (divisor 3),
> which is a safe, widely-compatible speed. 115200 would also work fine
> on QEMU, and many real boards use it for the console. We follow xv6
> here for consistency.

> **Why divide by 16?**
>
> The 16550 internally runs a clock at 16x the baud rate. For each bit
> period, the receiver samples the incoming signal 16 times and picks the
> sample near the middle. This oversampling helps reject noise — if a few
> of the 16 samples are corrupted by electrical interference, the middle
> sample is still likely correct. The "÷16" in the formula accounts for
> this 16x oversampling.

---

## Part 4: The Initialization Sequence

### What uart_init() must do

Before we can transmit or receive, we need to configure the UART. Here's
the sequence:

```
Step 1: Disable interrupts
        Write IER = 0x00
        ↓
Step 2: Enter baud rate programming mode
        Write LCR = 0x80 (set DLAB=1)
        ↓
Step 3: Set baud rate
        Write DLL = 0x03 (divisor low byte)
        Write DLM = 0x00 (divisor high byte)
        → baud = 1843200 / (16 × 3) = 38400
        ↓
Step 4: Set data format and exit DLAB mode
        Write LCR = 0x03 (8N1, DLAB=0)
        ↓
Step 5: Enable and reset FIFOs
        Write FCR = 0x07
        ↓
Step 6: (optional) Done — UART is ready for polling
```

Let's trace through each step and understand what's happening in the
hardware.

### Step 1: Disable interrupts (IER = 0x00)

```c
write_reg(IER, 0x00);
```

We start by turning off all UART interrupts. Even if the UART was
previously configured (maybe by firmware), we want a clean slate. Since
we're in Phase 1 with no trap handler, any interrupt would crash the
system.

### Step 2: Enter DLAB mode (LCR = 0x80)

```c
write_reg(LCR, 0x80);   // Set DLAB bit, everything else 0
```

```
LCR = 0x80 = 1000 0000
             ^
             DLAB = 1 → registers 0 and 1 now access the divisor latch
```

After this write, offsets 0x00 and 0x01 are no longer THR/RHR and IER —
they're now DLL and DLM. Any write to offset 0x00 goes to the divisor
low byte instead of transmitting a character.

### Step 3: Set baud rate (DLL = 0x03, DLM = 0x00)

```c
write_reg(0x00, 0x03);   // DLL = 3 (because DLAB=1, offset 0 is DLL)
write_reg(0x01, 0x00);   // DLM = 0
```

The 16-bit divisor is now `(DLM << 8) | DLL = (0 << 8) | 3 = 3`.

Baud rate = 1843200 / (16 × 3) = 38400.

### Step 4: Set data format (LCR = 0x03)

```c
write_reg(LCR, 0x03);   // 8 data bits, 1 stop bit, no parity, DLAB=0
```

```
LCR = 0x03 = 0000 0011
             ^      ^^
             DLAB=0 word length = 11 = 8 bits
```

This does two things at once: configures the data format (8N1) **and**
clears the DLAB bit, switching offsets 0x00 and 0x01 back to their normal
roles (THR/RHR and IER).

### Step 5: Enable FIFOs (FCR = 0x07)

```c
write_reg(FCR, 0x07);   // Enable FIFOs, reset both
```

```
FCR = 0x07 = 0000 0111
                   ^^^
                   ||└─ Enable FIFOs
                   |└── Clear RX FIFO
                   └─── Clear TX FIFO
```

The FIFO reset bits (1 and 2) are **self-clearing** — the hardware resets
the FIFOs and then automatically sets those bits back to 0. You don't need
to write FCR again to clear them.

### After initialization

The UART is now ready. You can:
- Write a byte to THR (offset 0x00) to transmit it
- Read LSR (offset 0x05) to check if TX is ready or RX data is available
- Read RHR (offset 0x00) to receive a byte

Let's visualize the complete state after init:

```
Register     Value    Meaning
───────────  ───────  ──────────────────────────────
IER (0x01)   0x00     All interrupts disabled
LCR (0x03)   0x03     8 data bits, 1 stop, no parity, DLAB=0
FCR (0x02)   0x01     FIFOs enabled (reset bits auto-cleared)
DLL (0x00*)  0x03     Divisor low = 3 (only accessible when DLAB=1)
DLM (0x01*)  0x00     Divisor high = 0
LSR (0x05)   0x60     TX empty + THR empty (ready to transmit)
```

---

## Part 5: Polling I/O — The Simple Approach

### What is polling?

There are two fundamental approaches for a CPU to interact with a device:

**Polling** — the CPU repeatedly checks ("polls") a status register in a
loop until the device is ready:

```c
while (device_not_ready())    // Check status register
    ;                          // Spin
do_the_operation();            // Device is ready, proceed
```

**Interrupt-driven** — the device sends a hardware interrupt to the CPU
when it's ready. The CPU does other work in the meantime:

```c
start_operation();             // Tell device to do something
return;                        // CPU goes back to other work
// ... later, when device is ready ...
// CPU receives interrupt, runs handler
interrupt_handler() {
    handle_completed_operation();
}
```

Here's a comparison:

```
Polling:                              Interrupt-driven:

CPU: "Ready yet?"                     CPU: "Let me know when you're done."
UART: "No."                           UART: (working...)
CPU: "Ready yet?"                     CPU: (does other work)
UART: "No."                           CPU: (does other work)
CPU: "Ready yet?"                     UART: *interrupt* "I'm done!"
UART: "No."                           CPU: "Great, let me handle that."
CPU: "Ready yet?"
UART: "Yes!"
CPU: "Great, here's the data."
```

| Approach | Pros | Cons |
|----------|------|------|
| Polling | Simple code, no trap handler needed, predictable latency | Wastes CPU cycles spinning, can't do other work while waiting |
| Interrupt-driven | CPU is free while device works, efficient for slow devices | Complex (needs trap handler, interrupt controller setup), latency to handle interrupt |

> **What is "latency to handle interrupt"?**
>
> When an interrupt fires, the CPU can't instantly run your handler. It
> must: finish the current instruction, save state (PC → `mepc`, cause →
> `mcause`), jump to the trap vector, save registers, figure out *which*
> device interrupted, call the right handler, then restore everything and
> `mret`. That's 50–200 CPU cycles of overhead per interrupt.
>
> With polling, the response is nearly instant — you're already in the
> loop checking the register. For a slow device like UART (one character
> every ~260 µs), 200 cycles of interrupt overhead is negligible. But for
> a high-speed network card receiving millions of packets per second, the
> overhead adds up — the CPU can spend more time entering/exiting the
> handler than doing actual work.
>
> Modern network drivers (like Linux's NAPI) solve this by **switching
> dynamically**: start with interrupts, and when packets flood in, the
> first interrupt disables further interrupts and switches to a polling
> loop. When the queue drains, re-enable interrupts. Best of both worlds:
>
> ```
> Light traffic:  interrupts — CPU sleeps between packets, wakes on demand
> Heavy traffic:  polling — process packets back-to-back, no overhead
> ```
>
> This is an advanced optimization we won't need for our UART, but it's
> good to know that polling vs interrupts isn't a permanent choice — real
> drivers blend both.

### Why polling is right for Phase 1

We use polling in Phase 1 for two reasons:

1. **We don't have a trap handler yet.** Interrupts require the CPU to jump
   to a trap handler when the device signals. We haven't written one (that's
   Phase 2). Without a trap handler, enabling UART interrupts would crash
   the system.

2. **We're only doing output.** Our Phase 1 kernel just prints messages and
   halts. It's not waiting for user input, running a scheduler, or doing
   anything else while printing. Spinning on the transmit-ready bit wastes
   no useful work — there's nothing else to do.

In Phase 2, we'll switch to interrupt-driven I/O. That's essential for
**input** — you don't know when the user will type a character, and you
don't want the CPU spinning in a loop forever waiting for keystrokes.

### Polling transmit

Here's the polling pattern for sending a character:

```c
void uart_putc(char c) {
    volatile uint8 *base = (volatile uint8 *)UART0;

    // Spin until the Transmit Holding Register is empty
    while ((base[LSR] & LSR_TX_IDLE) == 0)
        ;

    // THR is empty — write the character
    base[THR] = c;
}
```

On QEMU, the `while` loop almost never actually spins — the virtual UART
processes characters instantly. But on real hardware at 38400 baud, each
character takes about 260 microseconds (10 bits × 1/38400 seconds). If you
call `uart_putc` faster than that, you'll spin waiting.

> **10 bits per character?**
>
> Even though we configured 8 data bits, each character on the wire is
> framed with a start bit and stop bit: 1 + 8 + 1 = 10 bits total. At
> 38400 baud, that's 3840 characters per second, or about 260 microseconds
> per character.

### Polling receive

For receiving, the pattern is similar:

```c
int uart_getc(void) {
    volatile uint8 *base = (volatile uint8 *)UART0;

    // Check if data is available
    if ((base[LSR] & LSR_RX_READY) == 0)
        return -1;   // Nothing available

    // Read and return the byte
    return base[RHR];
}
```

Notice this is **non-blocking** — if no character is available, it returns
-1 immediately instead of spinning. This is a design choice. A blocking
version would spin like `uart_putc`, but for receive we prefer non-blocking
because the caller might want to do other things between checks.

---

## Part 6: How xv6 Does It

### xv6's UART driver structure

xv6's UART driver (`kernel/uart.c`) is more complex than what we need for
Phase 1, because it handles both polling and interrupts, plus output
buffering. Let's understand the full design so we know where we're headed.

xv6 defines its register offsets and constants:

```c
// kernel/uart.c (xv6)
#define RHR 0   // receive holding register (for input bytes)
#define THR 0   // transmit holding register (for output bytes)
#define IER 1   // interrupt enable register
#define FCR 2   // FIFO control register
#define ISR 2   // interrupt status register
#define LCR 3   // line control register
#define LSR 5   // line status register
```

And it accesses registers through a simple macro using the base address
from `memlayout.h`:

```c
#define UART0 0x10000000L
#define Reg(reg) ((volatile unsigned char *)(UART0 + reg))
#define ReadReg(reg) (*(Reg(reg)))
#define WriteReg(reg, v) (*(Reg(reg)) = (v))
```

This is the MMIO pattern we discussed: cast the register address to a
volatile pointer and dereference it.

### xv6's uart_init()

```c
void uartinit(void) {
    // disable interrupts.
    WriteReg(IER, 0x00);

    // special mode to set baud rate.
    WriteReg(LCR, LCR_BAUD_LATCH);

    // LSB for baud rate of 38.4K.
    WriteReg(0, 0x03);

    // MSB for baud rate of 38.4K.
    WriteReg(1, 0x00);

    // leave set-baud mode,
    // and set word length to 8 bits, no parity.
    WriteReg(LCR, LCR_EIGHT_BITS);

    // reset and enable FIFOs.
    WriteReg(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);

    // enable transmit and receive interrupts.
    WriteReg(IER, IER_TX_ENABLE | IER_RX_ENABLE);

    initlock(&uart_tx_lock, "uart");
}
```

This is almost identical to our initialization sequence, with two additions
at the end:
1. **Enable interrupts** — xv6 has a trap handler, so it can use
   interrupt-driven I/O
2. **Initialize a spinlock** — xv6 protects the UART with a lock because
   multiple CPUs might try to print simultaneously

### xv6's output: the transmit buffer

xv6 doesn't just write characters directly to THR. It uses a **ring
buffer**:

```c
// the transmit output buffer.
struct spinlock uart_tx_lock;
#define UART_TX_BUF_SIZE 32
char uart_tx_buf[UART_TX_BUF_SIZE];
uint64 uart_tx_w;  // write next to uart_tx_buf[uart_tx_w % ...]
uint64 uart_tx_r;  // read next from uart_tx_buf[uart_tx_r % ...]
```

```
Ring buffer (32 bytes):

  ┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐
  │   │   │ H │ e │ l │ l │ o │   │   │   │
  └───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘
            ↑                   ↑
          tx_r                tx_w
      (next to send)     (next to fill)
```

Why a buffer? Because the UART is slow compared to the CPU. If a process
calls `printf("Hello, world!\n")`, the CPU can generate all 14 characters
in microseconds, but the UART takes milliseconds to transmit them at 38400
baud. Without a buffer:

- The CPU would spin-wait for each character — extremely wasteful
- Other processes couldn't run while one process is printing

With the buffer:

1. `uartputc()` adds each character to the ring buffer
2. `uartstart()` sends the first character to the UART hardware
3. The CPU returns to do other work
4. When the UART finishes sending a character, it fires a **transmit
   complete interrupt**
5. The interrupt handler (`uartintr()`) calls `uartstart()` to send the
   next character from the buffer
6. Repeat until the buffer is empty

This is the **interrupt-driven** model — the CPU only touches the UART
when there's actually something to do (a buffer character to send, or a
received character to handle).

### xv6's input path

For input, xv6 uses a similar interrupt-driven approach:

1. User types a character
2. UART hardware puts it in the receive FIFO and fires an interrupt
3. `uartintr()` calls `uartgetc()` to read the byte
4. The byte is passed to `consoleintr()` which handles special keys
   (backspace, ctrl-U, etc.) and accumulates a line
5. When the user presses Enter, the accumulated line is made available
   to any process blocked on `read()`

### What we'll do differently in Phase 1

Our Phase 1 driver is much simpler:

| Feature | xv6 | bobchouOS Phase 1 |
|---------|-----|-------------------|
| Output method | Interrupt-driven with ring buffer | Polling (spin on THR empty) |
| Input method | Interrupt-driven via `consoleintr` | Polling (`uart_getc` returns -1 if nothing) |
| Concurrency | Spinlock protection | None (single hart, no interrupts) |
| Buffer | 32-byte transmit ring buffer | No buffer (direct to hardware) |
| Init | Same registers + enable interrupts | Same registers, interrupts disabled |

This is the right level of complexity for Phase 1. We'll evolve toward
xv6's design as we add features:
- Phase 2 adds traps → we can enable UART interrupts
- Phase 5 adds multiprocessing → we'll need the spinlock
- Phase 8 adds the console layer → we'll add line buffering

---

## Part 7: Our Driver Design

### The public interface

Our UART driver exposes three functions:

```c
// kernel/drivers/uart.h

void uart_init(void);     // Initialize the UART hardware
void uart_putc(char c);   // Send one character (blocking, polling)
int  uart_getc(void);     // Receive one character (non-blocking, -1 if none)
```

That's it. Three functions — init, put, get. This is the **minimum viable
UART driver**. `kprintf` (Round 3) will call `uart_putc` for each character
it formats. Interactive input (Phase 2+) will call `uart_getc`.

### Register definitions

We define register offsets and bit masks as named constants:

```c
// UART register offsets
#define RHR 0   // Receive Holding Register (read)
#define THR 0   // Transmit Holding Register (write)
#define IER 1   // Interrupt Enable Register
#define FCR 2   // FIFO Control Register (write)
#define LCR 3   // Line Control Register
#define LSR 5   // Line Status Register (read)

// LSR bit masks
#define LSR_RX_READY (1 << 0)   // Data available in RHR
#define LSR_TX_IDLE  (1 << 5)   // THR is empty, ready to send
```

We use `#define` constants instead of an enum or magic numbers. Each
register gets a name that matches the datasheet abbreviation, making the
code readable alongside hardware documentation.

### The register access pattern

Following xv6's approach, we define a helper for register access:

```c
#define UART0 0x10000000UL

// Read one UART register
static inline uint8
uart_read_reg(uint32 reg) {
    return *(volatile uint8 *)(UART0 + reg);
}

// Write one UART register
static inline void
uart_write_reg(uint32 reg, uint8 val) {
    *(volatile uint8 *)(UART0 + reg) = val;
}
```

The `static inline` means the compiler will replace each call with the
actual load/store instruction — no function call overhead. And `volatile`
ensures every access hits the hardware.

### How main.c changes

After implementing the driver, `main.c` becomes cleaner:

```c
// Before (Round 1 — raw UART hack):
static void uart_putc_raw(char c) {
    volatile uint8 *port = (volatile uint8 *)0x10000000;
    *port = c;
}
// ...
uart_puts_raw("hello from bobchouOS\n");

// After (Round 2 — proper driver):
#include "drivers/uart.h"
// ...
uart_init();
uart_putc('h');
uart_putc('e');
// ... or, with kprintf in Round 3:
kprintf("hello from bobchouOS\n");
```

The magic number `0x10000000` is now hidden inside the driver. The init
sequence is explicit. And the transmit-ready check ensures correctness on
real hardware.

---

## Part 8: Putting It All Together

### The complete initialization flow

Here's what happens when `kmain()` calls `uart_init()`:

```
kmain() calls uart_init()
    │
    ├── 1. IER = 0x00          Disable all UART interrupts
    │
    ├── 2. LCR = 0x80          Set DLAB=1 (enter baud rate mode)
    │
    ├── 3. DLL = 0x03          Divisor low byte = 3
    │      DLM = 0x00          Divisor high byte = 0
    │                          → Baud rate = 38400
    │
    ├── 4. LCR = 0x03          8 data bits, 1 stop, no parity, DLAB=0
    │
    └── 5. FCR = 0x07          Enable FIFOs, clear both

UART is now ready for polling I/O.
```

### The transmit flow

When `uart_putc('A')` is called:

```
uart_putc('A')
    │
    ├── Read LSR (offset 0x05)
    │   Is bit 5 (THR Empty) set?
    │     │
    │     ├── No → loop back, read LSR again (spin)
    │     │
    │     └── Yes → continue
    │
    └── Write 'A' (0x41) to THR (offset 0x00)
        │
        ▼
    UART hardware transmits the byte
    QEMU sends it to your terminal
    You see 'A' appear
```

### The receive flow

When `uart_getc()` is called:

```
uart_getc()
    │
    ├── Read LSR (offset 0x05)
    │   Is bit 0 (Data Ready) set?
    │     │
    │     ├── No → return -1 (nothing available)
    │     │
    │     └── Yes → Read RHR (offset 0x00)
    │                Return the byte
    │
    ▼
```

### Memory map perspective

Here's what the driver is actually doing in terms of memory addresses:

```
When uart_init() writes IER = 0x00:
  CPU executes:  sb zero, 1(t0)     where t0 = 0x10000000
  Address bus:   0x10000001
  Data bus:      0x00
  Hardware:      Bus routes to UART, UART's IER register is written

When uart_putc() reads LSR:
  CPU executes:  lb a0, 5(t0)       where t0 = 0x10000000
  Address bus:   0x10000005
  Data bus:      UART returns LSR value (e.g., 0x60 = TX empty)
  Hardware:      Bus routes to UART, UART's LSR register is read

When uart_putc() writes THR = 'A':
  CPU executes:  sb a1, 0(t0)       where t0 = 0x10000000
  Address bus:   0x10000000
  Data bus:      0x41 ('A')
  Hardware:      Bus routes to UART, byte enters transmit shift register
```

Each register access is just a load or store to a specific address in the
`0x10000000` – `0x10000007` range. The system bus routes it to the UART
instead of RAM. From the CPU's perspective, it's no different from accessing
memory.

---

## Part 9: Looking Ahead — From Polling to Interrupts

### Why we'll need interrupts

Polling works for Phase 1 because our kernel does nothing but print and
halt. But consider what happens when we have:

- A **scheduler** that switches between processes (Phase 5)
- A **shell** waiting for user input (Phase 8)
- **Multiple devices** that all need attention (UART + disk + timer)

With polling, you'd need something like:

```c
// Terrible polling loop
while (1) {
    if (uart_has_data())     check_uart();
    if (timer_expired())     switch_process();
    if (disk_done())         handle_disk();
    // ... check everything, constantly
}
```

This is **wasteful** (CPU spins doing nothing useful between checks) and
has **high latency** (you only notice a device is ready on the next loop
iteration). Interrupts solve both problems:

```c
// Interrupt-driven: CPU does useful work, devices interrupt when ready
schedule_next_process();
// ... CPU runs user code ...
// UART interrupt fires → uartintr() handles it
// Timer interrupt fires → schedule next process
// Disk interrupt fires → handle completed I/O
```

### What changes in Phase 2

When we implement trap handling in Phase 2, we'll modify the UART driver:

1. **Enable UART interrupts** — set IER bits 0 and 1
2. **Add `uart_intr()`** — called by the trap handler when a UART interrupt
   fires
3. **Add an output buffer** — so `uart_putc` doesn't spin-wait for each
   character
4. **Configure the PLIC** — the Platform-Level Interrupt Controller routes
   external device interrupts to the CPU

The transition from polling to interrupts is one of the most important
concepts in OS development. Phase 2 will cover it in detail.

### The driver evolution

Here's how our UART driver grows across phases:

```
Phase 1 (this round):
  uart_init()   — configure hardware, interrupts OFF
  uart_putc()   — polling transmit (spin on THR empty)
  uart_getc()   — polling receive (return -1 if nothing)

Phase 2 (traps):
  uart_init()   — same + enable interrupts
  uart_putc()   — buffered, interrupt-driven transmit
  uart_getc()   — same (called from interrupt handler now)
+ uart_intr()   — interrupt handler

Phase 5 (multiprocessing):
+ spinlock       — protect shared state from multiple CPUs

Phase 8 (console):
+ integration with console layer for line editing
```

Each phase adds complexity only when we have the infrastructure to support
it. Polling is correct and sufficient for Phase 1. Don't feel bad about
it — it's the same approach that bootloaders, BIOS/UEFI firmware, and early
Linux boot code use. Polling is "training wheels" only in the sense that a
bicycle is training wheels for a car — it's perfectly valid for its domain.

---

## What's Next

You now understand the 16550 UART hardware and how to write a driver for
it. Next steps for this round:

1. **I'll create the skeleton files** — `kernel/drivers/uart.h` (complete)
   and `kernel/drivers/uart.c` (with TODO markers for you to implement)
2. **You implement the TODOs** — the init sequence and polling functions
3. **I'll update `main.c`** — to use `uart_init()` and `uart_putc()`
   instead of the raw write
4. **We verify** — `make run` should still print "hello from bobchouOS"

---

## Quick Reference

### 16550 Register Map (Phase 1 subset)

| Offset | Name | R/W | Purpose |
|--------|------|-----|---------|
| 0x00 | RHR | R | Receive a byte (DLAB=0) |
| 0x00 | THR | W | Transmit a byte (DLAB=0) |
| 0x00 | DLL | R/W | Divisor low byte (DLAB=1) |
| 0x01 | IER | R/W | Interrupt enable (DLAB=0) |
| 0x01 | DLM | R/W | Divisor high byte (DLAB=1) |
| 0x02 | FCR | W | FIFO control |
| 0x03 | LCR | R/W | Line control (data format + DLAB) |
| 0x05 | LSR | R | Line status (TX ready, RX ready) |

### Key Constants

| Constant | Value | Meaning |
|----------|-------|---------|
| `UART0` | `0x10000000` | Base address on QEMU virt |
| `LSR_RX_READY` | `0x01` | Bit 0 of LSR: data available |
| `LSR_TX_IDLE` | `0x20` | Bit 5 of LSR: THR empty |
| Divisor for 38400 baud | `3` | DLL=3, DLM=0 |

### Initialization Sequence Summary

| Step | Register | Value | Purpose |
|------|----------|-------|---------|
| 1 | IER | 0x00 | Disable interrupts |
| 2 | LCR | 0x80 | Set DLAB=1 |
| 3 | DLL (offset 0) | 0x03 | Baud rate divisor low |
| 4 | DLM (offset 1) | 0x00 | Baud rate divisor high |
| 5 | LCR | 0x03 | 8N1, clear DLAB |
| 6 | FCR | 0x07 | Enable + reset FIFOs |

### UART Driver API

| Function | Behavior |
|----------|----------|
| `uart_init()` | Run init sequence, UART ready for polling |
| `uart_putc(c)` | Spin until THR empty, write `c` |
| `uart_getc()` | If data ready, return byte; else return -1 |

### Comparing with Round 1's raw approach

| | Round 1 (raw) | Round 2 (driver) |
|---|---|---|
| Init | None | Full 6-step sequence |
| TX ready check | None (assume always ready) | Poll LSR bit 5 |
| RX support | None | `uart_getc()` polls LSR bit 0 |
| Magic numbers | `0x10000000` in main.c | Hidden in uart.c |
| Works on real HW | Maybe (if UART happens to be pre-configured) | Yes |
