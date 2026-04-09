# entry.S — Bare-metal entry point (same as Lecture 0-1)

.section .text.entry
.global _start

_start:
    la sp, stack_top
    call main

spin:
    wfi
    j spin
