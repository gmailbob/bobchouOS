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
    # TODO: Save current callee-saved registers into old context (a0).
    #   sd ra,   0(a0)
    #   sd sp,   8(a0)
    #   sd s0,  16(a0)
    #   ... through s11 at 104(a0)

    # TODO: Restore callee-saved registers from new context (a1).
    #   ld ra,   0(a1)
    #   ld sp,   8(a1)
    #   ld s0,  16(a1)
    #   ... through s11 at 104(a1)

    # TODO: ret (jump to the newly loaded ra)

    ret   # placeholder — replace with full implementation
