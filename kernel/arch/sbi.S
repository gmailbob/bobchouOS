# sbi.S — S-mode ecall wrappers for requesting M-mode services.
#
# Normal kernel functions (S-mode, linked in kernel .text), written in
# assembly because the body is just "load a7, ecall, ret." The actual
# work happens in M-mode (m_vec.S) on the other side of the ecall.
#
# Arguments follow the standard calling convention: the compiler puts
# the first arg in a0, second in a1, etc. before calling these.
#
# See Lecture 5-1, Part 4.

#include "sbi.h"

.globl sbi_shutdown

# void sbi_shutdown(void)
#   Does not return (M-mode halts the machine).
sbi_shutdown:
    li    a7, SBI_SHUTDOWN
    ecall
    ret                        # unreachable
