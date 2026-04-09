# Lecture 0-3 Exercises: C Practice

Four C exercises to practice what you learned in the lecture.
All work happens in **`exercises.c`** — the other files are the test harness.
Reference solutions are in `solutions.c` (try before peeking).

## Setup

```bash
cd lectures/0-3-c-recap/exercises
make          # build
make qemu     # build and run (Ctrl-A then X to quit QEMU)
```

You should see 28 test cases, most showing FAIL. Your goal: make them all PASS.

## The exercises

Open `exercises.c`. You'll see four exercises with `/* TODO */` markers.

### Exercise 1: `bitfield_extract` / `bitfield_insert` — bit manipulation

Two functions that extract or insert a contiguous bit field within a
64-bit value.

- `bitfield_extract(val, start, width)` — return the `width` bits
  starting at bit position `start`
- `bitfield_insert(val, start, width, field)` — replace those bits
  with `field`, leaving everything else unchanged

This is the extract/insert pattern from Part 7, turned into actual code.
You need `>>`, `<<`, `&`, `|`, `~`, and the `1UL` trick for 64-bit masks.

### Exercise 2: `my_memset` / `my_memcpy` — pointer and byte access

Implement two fundamental memory functions that we can't get from libc:

- `my_memset(dst, val, n)` — fill `n` bytes with `val`
- `my_memcpy(dst, src, n)` — copy `n` bytes from `src` to `dst`

Both take `void *` parameters — you'll need to cast to `uint8 *` to do
byte-by-byte access (Part 4: casting, void *). Both return `dst`.

### Exercise 3: `sorted_insert` — structs and pointer-to-pointer

Insert a node into a sorted linked list, maintaining order. Uses the
`struct node **` pattern from Part 4:

- Walk with `struct node **pp` until you find the right position
- Splice in the new node

This is the "pointer to the link" pattern — the same code handles
inserting at the head, middle, or end with no special cases.

### Exercise 4: `format_hex` — everything together

Convert a `uint64` to a hex string like `"0x00000000DEADBEEF"`. This
combines:

- Bit shifting and masking (extract each 4-bit nibble)
- Array/pointer access (write characters into a buffer)
- ASCII arithmetic (`'0' + nibble` for 0-9, `'A' + nibble - 10` for A-F)

## Tips

- Exercise 1 is pure bit math — review Part 7's extract/insert formulas
- Exercise 2 is straightforward once you cast `void *` to `uint8 *`
- Exercise 3 is the trickiest — draw the pointer diagram on paper if
  the `**pp` pattern is confusing
- Exercise 4 is a loop over 16 nibbles — the only trick is the
  nibble-to-ASCII conversion
- After each edit, run `make qemu` to see which tests pass
- If stuck, re-read the relevant lecture section:
  - Exercise 1 → Part 7 (bitwise operations, extract/insert)
  - Exercise 2 → Part 4 (void *, casting) + Part 5 (pointer arithmetic)
  - Exercise 3 → Part 4 (pointer-to-pointer) + Part 6 (structs)
  - Exercise 4 → Part 7 (shifts, masks) + Part 5 (arrays)
