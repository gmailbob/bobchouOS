# kernel_vec.S — S-mode trap vector for bobchouOS
#
# When any S-mode trap occurs (interrupt, exception, ecall), the CPU
# jumps here (via stvec). This code saves ALL general-purpose registers
# onto the kernel stack, calls the C handler kernel_trap(), restores
# the registers, and returns via sret.
#
# We save all registers (not just caller-saved) because kernel_trap
# may call yield() → swtch(), which switches to another process.
# When we resume, swtch restores callee-saved regs from the OTHER
# process's yield context — so our original s0-s11 would be lost if
# we hadn't saved them here on the stack.
#
# Refer to Lectures 2-2 and 5-1.

.section .text
.globl kernel_vec
.align 4

kernel_vec:
    # ---- Save all general-purpose registers ----
    addi  sp, sp, -256

    sd    x1,    0(sp)    # ra
    sd    x3,    8(sp)    # gp
    sd    x5,   16(sp)    # t0
    sd    x6,   24(sp)    # t1
    sd    x7,   32(sp)    # t2
    sd    x8,   40(sp)    # s0/fp
    sd    x9,   48(sp)    # s1
    sd    x10,  56(sp)    # a0
    sd    x11,  64(sp)    # a1
    sd    x12,  72(sp)    # a2
    sd    x13,  80(sp)    # a3
    sd    x14,  88(sp)    # a4
    sd    x15,  96(sp)    # a5
    sd    x16, 104(sp)    # a6
    sd    x17, 112(sp)    # a7
    sd    x18, 120(sp)    # s2
    sd    x19, 128(sp)    # s3
    sd    x20, 136(sp)    # s4
    sd    x21, 144(sp)    # s5
    sd    x22, 152(sp)    # s6
    sd    x23, 160(sp)    # s7
    sd    x24, 168(sp)    # s8
    sd    x25, 176(sp)    # s9
    sd    x26, 184(sp)    # s10
    sd    x27, 192(sp)    # s11
    sd    x28, 200(sp)    # t3
    sd    x29, 208(sp)    # t4
    sd    x30, 216(sp)    # t5
    sd    x31, 224(sp)    # t6

    # ---- Call C trap handler, then check if scheduling is needed ----
    call kernel_trap
    call kernel_trap_ret

    # ---- Restore all general-purpose registers ----
    ld    x1,    0(sp)    # ra
    ld    x3,    8(sp)    # gp
    ld    x5,   16(sp)    # t0
    ld    x6,   24(sp)    # t1
    ld    x7,   32(sp)    # t2
    ld    x8,   40(sp)    # s0/fp
    ld    x9,   48(sp)    # s1
    ld    x10,  56(sp)    # a0
    ld    x11,  64(sp)    # a1
    ld    x12,  72(sp)    # a2
    ld    x13,  80(sp)    # a3
    ld    x14,  88(sp)    # a4
    ld    x15,  96(sp)    # a5
    ld    x16, 104(sp)    # a6
    ld    x17, 112(sp)    # a7
    ld    x18, 120(sp)    # s2
    ld    x19, 128(sp)    # s3
    ld    x20, 136(sp)    # s4
    ld    x21, 144(sp)    # s5
    ld    x22, 152(sp)    # s6
    ld    x23, 160(sp)    # s7
    ld    x24, 168(sp)    # s8
    ld    x25, 176(sp)    # s9
    ld    x26, 184(sp)    # s10
    ld    x27, 192(sp)    # s11
    ld    x28, 200(sp)    # t3
    ld    x29, 208(sp)    # t4
    ld    x30, 216(sp)    # t5
    ld    x31, 224(sp)    # t6

    addi  sp, sp, 256

    # ---- Return from trap ----
    sret
