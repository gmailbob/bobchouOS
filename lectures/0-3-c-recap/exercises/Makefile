# Makefile — Build and run Lecture 0-3 C exercises
#
# Usage:
#   make          Build the ELF binary
#   make qemu     Build and run on QEMU (Ctrl-A then X to quit)
#   make clean    Remove build artifacts

CROSS = riscv-none-elf-
CC    = $(CROSS)gcc
AS    = $(CROSS)gcc
LD    = $(CROSS)ld

CFLAGS  = -march=rv64imac -mabi=lp64 -ffreestanding -nostdlib -mcmodel=medany -Wall -O2 -g
ASFLAGS = -march=rv64imac -mabi=lp64 -g

TARGET = exercises.elf
OBJS   = entry.o exercises.o main.o

QEMU   = qemu-system-riscv64
QFLAGS = -machine virt -nographic -bios none -kernel $(TARGET)

all: $(TARGET)

$(TARGET): $(OBJS) linker.ld
	$(LD) -m elf64lriscv -T linker.ld -o $@ $(OBJS)

entry.o: entry.S
	$(AS) $(ASFLAGS) -c -o $@ $<

exercises.o: exercises.c
	$(CC) $(CFLAGS) -c -o $@ $<

main.o: main.c
	$(CC) $(CFLAGS) -c -o $@ $<

qemu: $(TARGET)
	$(QEMU) $(QFLAGS)

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all qemu clean
