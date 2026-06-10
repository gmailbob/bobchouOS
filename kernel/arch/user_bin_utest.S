/*
 * user_bin_utest.S — Embed user/utest ELF into kernel .rodata.
 *
 * See Lecture 6-3, Part 5.
 */

.section .rodata
.globl _binary_utest_start
.globl _binary_utest_end
_binary_utest_start:
    .incbin "user/utest.elf"
_binary_utest_end:
