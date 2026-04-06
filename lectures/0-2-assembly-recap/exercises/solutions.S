# solutions.S — Reference solutions for Lecture 0-2 exercises
#
# For each exercise: your solution, then an alternative approach.
# Both are correct — the differences illustrate that assembly often
# has multiple valid ways to express the same logic.
#
# This file is not built by the Makefile. It's read-only reference.

.section .text

# ═════════════════════════════════════════════════════════════════════
# Exercise 1: add_three
# ═════════════════════════════════════════════════════════════════════

# Your solution:
#
#   add a0, a0, a1          # a0 = a + b
#   add a0, a0, a2          # a0 = (a + b) + c
#   ret
#
# No meaningful alternative — two adds is the only way.

# ═════════════════════════════════════════════════════════════════════
# Exercise 2: strlen
# ═════════════════════════════════════════════════════════════════════

# Your solution — explicit counter:
#
#   mv   t0, a0             # t0 = pointer (free up a0 for counter)
#   mv   a0, zero           # a0 = count = 0
# check_end:
#   lbu  t1, 0(t0)          # load byte
#   bne  t1, zero, add_count
#   ret
# add_count:
#   addi a0, a0, 1          # count++
#   addi t0, t0, 1          # pointer++
#   j    check_end
#
# Alternative — pointer subtraction (no counter register):
#
#   mv   t0, a0             # t0 = start of string
# .loop:
#   lbu  t1, 0(a0)          # load byte at current position
#   beqz t1, .done          # if null, stop
#   addi a0, a0, 1          # advance pointer
#   j    .loop
# .done:
#   sub  a0, a0, t0         # length = end position - start
#   ret
#
# Comparison: yours tracks a counter (a0) and pointer (t0) separately.
# The alternative walks a0 forward and computes the length at the end
# via subtraction — one fewer register to increment in the loop, but
# needs a sub at the end.
#
# Style note: your version has "ret" in the middle of the function
# (between check_end and add_count), while the alternative has a single
# "ret" at the bottom. Both work, but single-exit-at-the-bottom is
# easier to follow as functions grow — you read top-to-bottom without
# tracking multiple exit paths.
#
# The key technique: flip the branch condition. Instead of branching to
# the "keep going" path (bne → jump to work), branch to the "stop"
# path (beqz → jump to exit). The loop body falls through naturally,
# and the exit sits at the bottom. This is the same inversion the
# compiler uses for C "if" statements (see Part 3 of the lecture).

# ═════════════════════════════════════════════════════════════════════
# Exercise 3: max_array
# ═════════════════════════════════════════════════════════════════════

# Your solution — index counter:
#
#   mv   t0, a0             # t0 = pointer
#   ld   a0, 0(t0)          # a0 = max = arr[0]
#   addi t0, t0, 8          # advance to arr[1]
#   li   t1, 1              # t1 = index = 1
# check_index:
#   blt  t1, a1, set_max    # if index < n, continue
#   ret
# set_max:
#   ld   t2, 0(t0)          # t2 = arr[index]
#   addi t0, t0, 8          # advance pointer
#   addi t1, t1, 1          # index++
#   blt  t2, a0, check_index  # if current < max, skip update
#   mv   a0, t2             # max = current
#   j    check_index
#
# Alternative — end pointer instead of index:
#
#   slli t0, a1, 3          # t0 = n * 8 (byte length)
#   add  t0, a0, t0         # t0 = end pointer (past last element)
#   ld   t1, 0(a0)          # t1 = max = arr[0]
#   addi a0, a0, 8          # advance to arr[1]
# .loop:
#   bge  a0, t0, .done      # if pointer >= end, stop
#   ld   t2, 0(a0)          # t2 = current element
#   addi a0, a0, 8          # advance pointer
#   bge  t1, t2, .loop      # if max >= current, skip update
#   mv   t1, t2             # max = current
#   j    .loop
# .done:
#   mv   a0, t1             # return max
#   ret
#
# Comparison: yours uses an index (t1) compared against n — maps
# directly to "for (i = 1; i < n; i++)" and is easy to read. The
# alternative precomputes an end pointer, avoiding the index register
# but needing a setup shift. Same loop cost, different style.

# ═════════════════════════════════════════════════════════════════════
# Exercise 4: sum_strlen
# ═════════════════════════════════════════════════════════════════════

# Your solution — textbook prologue/epilogue:
#
#   addi sp, sp, -32        # prologue: allocate 32 bytes
#   sd   ra, 24(sp)         # save ra (call will overwrite it)
#   sd   s0, 16(sp)         # save s0 (we'll use it for strlen(s1))
#   sd   s1, 8(sp)          # save s1 (we'll use it for s2)
#
#   mv   s1, a1             # s1 = s2 (preserve across first call)
#   call strlen              # a0 = strlen(s1) — a0 already has s1
#   mv   s0, a0             # s0 = strlen(s1) (preserve across second call)
#   mv   a0, s1             # set up arg: a0 = s2
#   call strlen              # a0 = strlen(s2)
#   add  a0, a0, s0         # a0 = strlen(s1) + strlen(s2)
#
#   ld   s1, 8(sp)          # epilogue: restore s1
#   ld   s0, 16(sp)         # restore s0
#   ld   ra, 24(sp)         # restore ra
#   addi sp, sp, 32         # deallocate frame
#   ret
#
# This is the sum_of_squares pattern from the lecture. The calling
# convention dictates the structure — no meaningful alternative.
