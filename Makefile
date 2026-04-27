# bobchouOS — Root Makefile
#
# Usage:
#   make          Build kernel.elf
#   make run      Build + launch QEMU
#   make test     Build with tests + launch QEMU
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

ASFLAGS = -march=rv64imac_zicsr -mabi=lp64 -g \
          -I kernel/include -I kernel

TARGET  = kernel.elf

OBJS    = kernel/arch/entry.o \
          kernel/arch/kernel_vec.o \
          kernel/main.o \
          kernel/trap.o \
          kernel/kalloc.o \
          kernel/drivers/uart.o \
          kernel/lib/string.o \
          kernel/lib/kprintf.o

TEST_OBJS = kernel/test/run_tests.o \
            kernel/test/test_kprintf.o \
            kernel/test/test_string.o \
            kernel/test/test_trap.o \
            kernel/test/test_kalloc.o

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

# Test build: clean first so main.o is recompiled with -DRUN_TESTS,
# then build kernel.elf with test objects linked in.
test: clean
	$(MAKE) CFLAGS="$(CFLAGS) -DRUN_TESTS" \
	        OBJS="$(OBJS) $(TEST_OBJS)" \
	        $(TARGET)
	$(QEMU) $(QFLAGS)

debug: $(TARGET)
	$(QEMU) $(QFLAGS) -s -S

clean:
	rm -f $(OBJS) $(TEST_OBJS) $(TARGET)

.PHONY: all run debug test clean
