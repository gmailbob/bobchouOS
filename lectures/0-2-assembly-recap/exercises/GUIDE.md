# Lecture 0-2 Exercises: Assembly Practice

Four assembly exercises to practice what you learned in the lecture.
All work happens in **`exercises.S`** — the other files are the test harness.
Reference solutions are in `solutions.S` (try before peeking).

## Setup

```bash
cd lectures/0-2-assembly-recap/exercises
make          # build
make qemu     # build and run (Ctrl-A then X to quit QEMU)
```

You should see 20 test cases, most showing FAIL. Your goal: make them all PASS.

## The exercises

Open `exercises.S`. You'll see four functions with `# TODO` markers.

### Exercise 1: `add_three(a, b, c)` — return `a + b + c`

- Arguments arrive in `a0`, `a1`, `a2`
- Put the result in `a0`
- This is 2 instructions

### Exercise 2: `strlen(s)` — count characters in a string

- Pointer to the string is in `a0`
- Load one byte at a time with `lbu`
- Loop until you find `'\0'` (zero byte)
- Return the count in `a0`
- You'll need a loop, a branch, and a counter

### Exercise 3: `max_array(arr, n)` — find the largest element

- Array pointer in `a0`, length in `a1` (guaranteed `n >= 1`)
- Each element is a `long` (8 bytes) — use `ld`
- Return the largest value in `a0`
- You'll need a loop with a conditional branch inside (compare current
  element against your running max)

### Exercise 4: `sum_strlen(s1, s2)` — return `strlen(s1) + strlen(s2)`

- You must **call your `strlen`** from Exercise 2 (twice)
- This is a **non-leaf function** — `call` overwrites `ra`, so you need
  a prologue/epilogue
- Save `ra`, preserve `s2` and the first result across calls using
  `s`-registers
- This follows the exact `sum_of_squares` pattern from the lecture
- Only works if Exercise 2 is correct first

## Tips

- Exercises 1-3 are **leaf functions** — no `call` inside, so no
  prologue/epilogue needed (no saving `ra` or `s` registers).
- Exercise 4 is a **non-leaf function** — you need prologue/epilogue.
- Use `t0`-`t6` for scratch values in leaf functions, `s0`-`s11` for
  values that must survive across `call` in non-leaf functions.
- After each edit, run `make qemu` to see which tests pass.
- If stuck, re-read the relevant section of the lecture:
  - Exercise 1 → Part 2 (registers, calling convention basics)
  - Exercise 2 → Part 2 (load/store) + Part 3 (branches, loops)
  - Exercise 3 → Part 2 (load/store) + Part 3 (branches, comparison)
  - Exercise 4 → Part 2 (stack, Example 2) + Part 5 (calling convention)
