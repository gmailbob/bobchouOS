# sbi.S — S-mode ecall wrappers for requesting M-mode services.
#
# These are normal kernel functions (S-mode, linked in kernel .text),
# written in assembly. The actual work happens in M-mode (m_vec.S) on
# the other side of the ecall boundary.

# Called from S-mode C code (scheduler, test runner, etc.) like any
# other function. Arguments follow the standard calling convention:
# compiler will put 1st arg to a0, 2nd to a1, etc.
#
# See Lecture 5-1, Part 4.

#include "sbi.h"

.globl sbi_set_timer
.globl sbi_shutdown

# sbi_set_timer(uint64 deadline)
#   a0 = deadline (already in place per calling convention)
#   Traps to M-mode which writes a0 to mtimecmp.
sbi_set_timer:
    li    a7, SBI_SET_TIMER
    ecall
    ret

# sbi_shutdown(void)
#   Traps to M-mode which writes to QEMU shutdown device.
#   Does not return.
sbi_shutdown:
    li    a7, SBI_SHUTDOWN
    ecall
    ret   # unreachable
