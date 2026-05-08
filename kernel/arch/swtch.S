# swtch.S — Context switch between two execution contexts.
#
# void swtch(struct context *old, struct context *new);
#   a0 = pointer to context to save into (old)
#   a1 = pointer to context to restore from (new)
#
# Saves 14 registers (ra, sp, s0-s11) to *old, restores from *new,
# then ret jumps to new->ra. After this returns, we are on a different
# stack, in a different function. See Lecture 5-1, Part 3.
#
# Offsets must match struct context field order in proc.h:
#   0(ctx)  = ra
#   8(ctx)  = sp
#   16(ctx) = s0
#   24(ctx) = s1
#   ...
#   104(ctx) = s11

.globl swtch

swtch:
    # Save current callee-saved registers into old context (a0).
    sd    x1,    0(a0)    # ra
    sd    x2,    8(a0)    # sp
    sd    x8,   16(a0)    # s0
    sd    x9,   24(a0)    # s1
    sd    x18,  32(a0)    # s2
    sd    x19,  40(a0)    # s3
    sd    x20,  48(a0)    # s4
    sd    x21,  56(a0)    # s5
    sd    x22,  64(a0)    # s6
    sd    x23,  72(a0)    # s7
    sd    x24,  80(a0)    # s8
    sd    x25,  88(a0)    # s9
    sd    x26,  96(a0)    # s10
    sd    x27, 104(a0)    # s11

    # Restore callee-saved registers from new context (a1).
    ld    x1,    0(a1)    # ra
    ld    x2,    8(a1)    # sp
    ld    x8,   16(a1)    # s0
    ld    x9,   24(a1)    # s1
    ld    x18,  32(a1)    # s2
    ld    x19,  40(a1)    # s3
    ld    x20,  48(a1)    # s4
    ld    x21,  56(a1)    # s5
    ld    x22,  64(a1)    # s6
    ld    x23,  72(a1)    # s7
    ld    x24,  80(a1)    # s8
    ld    x25,  88(a1)    # s9
    ld    x26,  96(a1)    # s10
    ld    x27, 104(a1)    # s11

    # jump to the newly loaded ra
    ret
