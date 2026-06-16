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
          -I include -I kernel/include -I kernel

ASFLAGS = -march=rv64imac_zicsr -mabi=lp64 -g \
          -I include -I kernel/include -I kernel

TARGET  = kernel.elf

OBJS    = kernel/arch/entry.o \
          kernel/arch/kernel_vec.o \
          kernel/arch/m_vec.o \
          kernel/arch/swtch.o \
          kernel/arch/sbi.o \
          kernel/arch/trampoline.o \
          kernel/arch/user_bin_init.o \
          kernel/arch/user_bin_utest.o \
          kernel/main.o \
          kernel/trap.o \
          kernel/kalloc.o \
          kernel/vm.o \
          kernel/vm_fault.o \
          kernel/kmalloc.o \
          kernel/proc.o \
          kernel/vma.o \
          kernel/exec.o \
          kernel/syscall.o \
          kernel/spinlock.o \
          kernel/wait_queue.o \
          kernel/drivers/uart.o \
          kernel/drivers/plic.o \
          kernel/drivers/virtio_blk.o \
          kernel/lib/string.o \
          kernel/lib/kprintf.o

TEST_OBJS = kernel/test/run_unit_tests.o \
            kernel/test/run_integration_tests.o \
            kernel/test/unit/test_kprintf.o \
            kernel/test/unit/test_string.o \
            kernel/test/unit/test_trap.o \
            kernel/test/unit/test_kalloc.o \
            kernel/test/unit/test_vm.o \
            kernel/test/unit/test_kmalloc.o \
            kernel/test/unit/test_list.o \
            kernel/test/unit/test_hashtable.o \
            kernel/test/unit/test_spinlock.o \
            kernel/test/unit/test_vma.o \
            kernel/test/integration/test_proc.o \
            kernel/test/integration/test_wait_queue.o \
            kernel/test/integration/test_virtio_blk.o \
            kernel/test/integration/test_spinlock_contention.o

QEMU    = qemu-system-riscv64

# Disk image backing the virtio-blk device. Plain raw file; mkfs will
# format it for real in Round 7-4. For now it just needs to exist so
# the device has something to DMA against (smoke test reads block 0).
DISK    = fs.img
DISK_MB = 16

# virtio-blk over MMIO (QEMU virt wires virtio-mmio-bus.0 at VIRTIO0_BASE).
# if=none + -device keeps it off the default PCI bus so it lands on MMIO.
# -cpu rv64,sstc=on: pin SSTC (default-on, but documents the dependency and
# guards against a QEMU default change). See lectures/stretch-sstc-timer.
QFLAGS  = -machine virt -cpu rv64,sstc=on -nographic -bios none -kernel $(TARGET) \
          -global virtio-mmio.force-legacy=false \
          -drive file=$(DISK),if=none,format=raw,id=x0 \
          -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0

USER_PROGS = user/init.elf user/utest.elf

# ---------- User program build ----------

USER_CFLAGS = -march=rv64imac_zicsr -mabi=lp64 \
              -ffreestanding -nostdlib -mcmodel=medany \
              -Wall -O2 -g -I include -I user

user/init.elf: user/init.c user/start.S user/usys.S user/user.ld user/user.h include/syscall_num.h
	$(CC) $(USER_CFLAGS) -nostdlib -T user/user.ld -o $@ user/start.S user/usys.S user/init.c

user/utest.elf: user/utest.c user/start.S user/usys.S user/user.ld user/user.h include/syscall_num.h
	$(CC) $(USER_CFLAGS) -nostdlib -T user/user.ld -o $@ user/start.S user/usys.S user/utest.c

kernel/arch/user_bin_init.o: kernel/arch/user_bin_init.S user/init.elf
	$(AS) $(ASFLAGS) -c -o $@ $<

kernel/arch/user_bin_utest.o: kernel/arch/user_bin_utest.S user/utest.elf
	$(AS) $(ASFLAGS) -c -o $@ $<

# ---------- Targets ----------

all: $(TARGET)

$(TARGET): $(OBJS) linker.ld
	$(LD) -m elf64lriscv -T linker.ld -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(AS) $(ASFLAGS) -c -o $@ $<

# Disk image: create a zeroed raw file if it doesn't exist. Round 7-4's
# mkfs will populate it with a real filesystem; until then it's blank.
$(DISK):
	dd if=/dev/zero of=$(DISK) bs=1M count=$(DISK_MB) 2>/dev/null

run: $(TARGET) $(DISK)
	$(QEMU) $(QFLAGS)

# Test build: clean first so main.o is recompiled with -DRUN_TESTS,
# then build kernel.elf with test objects linked in.
test: clean $(DISK)
	$(MAKE) CFLAGS="$(CFLAGS) -DRUN_TESTS" \
	        OBJS="$(OBJS) $(TEST_OBJS)" \
	        $(TARGET)
	$(QEMU) $(QFLAGS)

debug: $(TARGET) $(DISK)
	$(QEMU) $(QFLAGS) -s -S

# Note: clean does NOT remove $(DISK) — it's data, not a build artifact.
# Use `make clean-disk` to discard it.
clean:
	rm -f $(OBJS) $(TEST_OBJS) $(TARGET) $(USER_PROGS)

clean-disk:
	rm -f $(DISK)

.PHONY: all run debug test clean clean-disk
