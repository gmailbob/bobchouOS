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
          kernel/lib/string.o \
          kernel/lib/kprintf.o

TEST_OBJS = kernel/test/run_tests.o \
            kernel/test/test_kprintf.o \
            kernel/test/test_string.o \
            kernel/test/test_trap.o \
            kernel/test/test_kalloc.o \
            kernel/test/test_vm.o \
            kernel/test/test_kmalloc.o \
            kernel/test/test_list.o \
            kernel/test/test_hashtable.o \
            kernel/test/test_spinlock.o \
            kernel/test/test_wait_queue.o \
            kernel/test/test_proc.o \
            kernel/test/test_vma.o

QEMU    = qemu-system-riscv64
QFLAGS  = -machine virt -nographic -bios none -kernel $(TARGET)

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
	rm -f $(OBJS) $(TEST_OBJS) $(TARGET) $(USER_PROGS)

.PHONY: all run debug test clean
