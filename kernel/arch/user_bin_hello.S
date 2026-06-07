/*
 * user_bin_hello.S — Embed user/hello ELF into kernel .rodata.
 *
 * See Lecture 6-3, Part 5.
 */

.section .rodata
.globl _binary_hello_start
.globl _binary_hello_end
_binary_hello_start:
    .incbin "user/hello.elf"
_binary_hello_end:
