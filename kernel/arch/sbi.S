# sbi.S — Mini-SBI ecall wrappers (S-mode side).
#
# Each function loads a7 with the SBI function ID and issues ecall.
# M-mode handler (m_vec.S) dispatches on a7 and performs the operation.
#
# These are called as normal C functions from S-mode kernel code.
# Arguments follow the standard calling convention (a0, a1, ...).
#
# See Lecture 5-1, Part 4.

#include "sbi.h"

.globl sbi_set_timer
.globl sbi_shutdown

# sbi_set_timer(uint64 deadline)
#   a0 = deadline (already in place per calling convention)
#   Traps to M-mode which writes a0 to mtimecmp.
sbi_set_timer:
    # TODO: load SBI_SET_TIMER into a7, then ecall, then ret.

    ret   # placeholder

# sbi_shutdown(void)
#   Traps to M-mode which writes to QEMU shutdown device.
#   Does not return.
sbi_shutdown:
    # TODO: load SBI_SHUTDOWN into a7, then ecall.
    # (M-mode halts the machine — ret is unreachable, but include it
    # for safety in case M-mode ever returns.)

    # Temporary: direct MMIO shutdown so tests work before m_vec is implemented.
    li   t0, 0x100000
    li   t1, 0x5555
    sw   t1, 0(t0)

    ret   # unreachable
