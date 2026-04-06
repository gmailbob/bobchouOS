# exercises.S — Assembly exercises for Lecture 0-2
#
# Implement the four functions below. Each one is called from main.c
# which prints PASS or FAIL for each test case.
#
# Rules:
#   - Exercises 1-3 are leaf functions (no "call" needed, no prologue/epilogue)
#   - Exercise 4 is a non-leaf function (you need prologue/epilogue)
#   - Arguments arrive in a0, a1, a2 (per calling convention)
#   - Return value goes in a0
#   - Use t0-t6 for scratch in leaf functions, s0-s11 for values that
#     must survive across "call" in non-leaf functions
#   - Look at the C signature above each function for what it should do

.section .text

# ─────────────────────────────────────────────────────────────────────
# Exercise 1: add_three
#
# C signature:  long add_three(long a, long b, long c);
# Returns:      a + b + c
#
# Hint: arguments are in a0, a1, a2. Result goes in a0.
# ─────────────────────────────────────────────────────────────────────
.global add_three
add_three:
    # TODO: write your code here


    ret

# ─────────────────────────────────────────────────────────────────────
# Exercise 2: strlen
#
# C signature:  long strlen(const char *s);
# Returns:      number of characters before the null terminator ('\0')
#
# Example: strlen("Hi") = 2, strlen("") = 0
#
# Hint: use lbu to load one byte at a time.
#       Compare each byte with zero to find the end.
#       The pointer starts in a0.
# ─────────────────────────────────────────────────────────────────────
.global strlen
strlen:
    # TODO: write your code here


    ret

# ─────────────────────────────────────────────────────────────────────
# Exercise 3: max_array
#
# C signature:  long max_array(const long *arr, long n);
# Returns:      the largest element in the array
#               (you may assume n >= 1)
#
# Example: max_array([3, 7, 2], 3) = 7
#
# Hint: arr pointer is in a0, length n is in a1.
#       Each element is 8 bytes (use ld).
#       Start with the first element as your "current max",
#       then loop through the rest comparing with blt/bge.
# ─────────────────────────────────────────────────────────────────────
.global max_array
max_array:
    # TODO: write your code here


    ret

# ─────────────────────────────────────────────────────────────────────
# Exercise 4: sum_strlen
#
# C signature:  long sum_strlen(const char *s1, const char *s2);
# Returns:      strlen(s1) + strlen(s2)
#
# You MUST call your strlen function from Exercise 2 (twice).
# This means you need a prologue/epilogue — call overwrites ra,
# and the first call trashes a0 (the return value replaces it).
#
# Hint: this follows the same pattern as sum_of_squares in the
#       lecture. You need to save ra, and preserve s2 and the
#       first result across calls. Use s-registers.
# ─────────────────────────────────────────────────────────────────────
.global sum_strlen
sum_strlen:
    # TODO: write your prologue, body, and epilogue here


    ret
