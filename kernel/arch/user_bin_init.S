/*
 * user_bin_init.S — Embed user/init ELF into kernel .rodata.
 *
 * See Lecture 6-3, Part 5.
 */

.section .rodata
.globl _binary_init_start
.globl _binary_init_end
_binary_init_start:
    .incbin "user/init"
_binary_init_end:
