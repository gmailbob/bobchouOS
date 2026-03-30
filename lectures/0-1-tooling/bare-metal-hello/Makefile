# Makefile — Build and run bare-metal Hello World on QEMU
#
# Usage:
#   make          Build the ELF binary
#   make run      Build and boot on QEMU (Ctrl-A then X to quit)
#   make debug    Boot QEMU paused, waiting for GDB to attach
#   make clean    Remove build artifacts

# --- Toolchain ---
# xPack uses "riscv-none-elf-" prefix (same as "riscv64-unknown-elf-" functionally)
CROSS = riscv-none-elf-
CC    = $(CROSS)gcc
AS    = $(CROSS)gcc        # Use gcc as assembler (handles .S preprocessing)
LD    = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

# --- Flags ---
# -march=rv64imac   Target instruction set: 64-bit base (I), multiply (M),
#                   atomics (A), compressed (C). Matches QEMU "virt" machine.
# -mabi=lp64       ABI: "long and pointers are 64-bit", no hardware float.
# -ffreestanding   Tell GCC there's no OS: don't assume libc exists.
# -nostdlib        Don't link the C standard library or startup files.
# -mcmodel=medany  Allow code/data to be placed anywhere in the address space.
#                  (Default "medlow" only works for addresses near 0.)
CFLAGS  = -march=rv64imac -mabi=lp64 -ffreestanding -nostdlib -mcmodel=medany -Wall -O2 -g
ASFLAGS = -march=rv64imac -mabi=lp64 -g

# --- Files ---
TARGET = hello.elf
OBJS   = entry.o main.o

# --- QEMU ---
QEMU  = qemu-system-riscv64
# -machine virt     Use the "virt" platform (provides UART at 0x10000000)
# -nographic        No graphical window; serial I/O goes to terminal
# -bios none        Don't load OpenSBI firmware; jump directly to our code
# -kernel $(TARGET) Load our ELF and jump to its entry point
QFLAGS = -machine virt -nographic -bios none -kernel $(TARGET)

# --- Rules ---
all: $(TARGET)

$(TARGET): $(OBJS) linker.ld
	$(LD) -m elf64lriscv -T linker.ld -o $@ $(OBJS)

entry.o: entry.S
	$(AS) $(ASFLAGS) -c -o $@ $<

main.o: main.c
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(TARGET)
	$(QEMU) $(QFLAGS)

# Debug mode: QEMU pauses at startup, waiting for GDB on port 1234.
#   -s  shorthand for -gdb tcp::1234
#   -S  freeze CPU at startup (waits for GDB "continue" command)
debug: $(TARGET)
	$(QEMU) $(QFLAGS) -s -S

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all run debug clean
