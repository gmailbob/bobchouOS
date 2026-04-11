# bobchouOS — Root Makefile
#
# Usage:
#   make          Build kernel.elf
#   make run      Build + launch QEMU
#   make debug    Build + launch QEMU with GDB stub (-s -S)
#   make clean    Remove build artifacts

CROSS   = riscv-none-elf-
CC      = $(CROSS)gcc
AS      = $(CROSS)gcc
LD      = $(CROSS)ld
OBJDUMP = $(CROSS)objdump
NM      = $(CROSS)nm

CFLAGS  = -march=rv64imac_zicsr -mabi=lp64 \
          -ffreestanding -nostdlib -mcmodel=medany \
          -Wall -O2 -g \
          -fno-strict-aliasing \
          -I kernel/include -I kernel

ASFLAGS = -march=rv64imac_zicsr -mabi=lp64 -g

TARGET  = kernel.elf

OBJS    = kernel/arch/entry.o \
          kernel/main.o \
          kernel/drivers/uart.o \
          kernel/lib/string.o \
          kernel/lib/kprintf.o

QEMU    = qemu-system-riscv64
QFLAGS  = -machine virt -nographic -bios none -kernel $(TARGET)

# ---------- Targets ----------

all: $(TARGET)

$(TARGET): $(OBJS) linker.ld
	$(LD) -m elf64lriscv -T linker.ld -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(AS) $(ASFLAGS) -c -o $@ $<

run: $(TARGET)
	$(QEMU) $(QFLAGS)

debug: $(TARGET)
	$(QEMU) $(QFLAGS) -s -S

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all run debug clean
