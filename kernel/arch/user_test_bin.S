# user_test_bin.S — Embed the test user binary into the kernel.
#
# The binary (compiled from user/test_user.S) is included as raw bytes
# in .rodata. Accessed from C via test_user_bin / test_user_bin_end.
#
# See Lecture 6-1, Part 8.

.section .rodata
.globl test_user_bin
.align 4
test_user_bin:
    .incbin "user/test_user.bin"

.globl test_user_bin_end
test_user_bin_end:
