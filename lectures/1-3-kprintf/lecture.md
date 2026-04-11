# Lecture 1-3: kprintf — Formatted Output for the Kernel

> **Where we are.**
> The UART driver is working. `uart_putc()` sends one character to the
> serial console, `uart_getc()` receives one, and we wrote a small
> `uart_puts()` helper in `main.c` to print whole strings. We can say
> "hello from bobchouOS" and see it on screen.
>
> But that is not enough. A kernel needs to print numbers, hex addresses,
> register values, and diagnostic messages — and it needs to do this
> *concisely* from C code. Writing `uart_putc('0'); uart_putc('x'); ...`
> for every debug message is not practical.
>
> This round we build `kprintf()`, a minimal `printf`-style function
> that formats strings and calls `uart_putc()` for output. It is the
> single most important debugging tool we will have for the rest of this
> project.

**xv6 book coverage:** Chapter 5 ("Interrupts and device drivers"),
section "Code: Console output" (p.54) describes the kernel's printf
implementation. The actual source is `kernel/printf.c` in xv6-riscv.
Our kprintf covers the same formatting logic but omits the spinlock
(we will add locking when we build the scheduler in Phase 2).

---

## Part 1: Why Formatted Output Matters

### 1.1 The problem with raw character output

Right now, if we want to print the value of a register, we have to
convert it to characters manually:

```c
/* Printing the number 42 without printf — painful. */
uart_putc('4');
uart_putc('2');
uart_putc('\n');
```

That works for hardcoded values, but what about variables? What about
addresses? What about printing an error code returned from a function?
We would need to write integer-to-string conversion code every single
time.

Every real operating system solves this problem the same way: a
`printf`-like function that accepts a **format string** with
**conversion specifiers**, plus a variable number of arguments to fill
in those specifiers.

### 1.2 Kernel printf vs. userspace printf

You have used `printf()` from the C standard library (`<stdio.h>`)
many times. That function eventually calls the `write()` system call,
which asks the kernel to move bytes to a file descriptor.

But we *are* the kernel. There is no `write()` system call we can
invoke on ourselves. We need our own implementation that writes
directly to the hardware. That is why kernel printf has a different
name in many systems:

| System       | Kernel print function | Output target          |
|--------------|-----------------------|------------------------|
| Linux        | `printk()`            | kernel log buffer      |
| xv6          | `printf()`            | console device         |
| FreeBSD      | `printf()` / `kprintf()` | console device     |
| bobchouOS    | `kprintf()`           | UART via `uart_putc()` |

We call ours `kprintf()` (kernel printf) to make it clear that this
is not the C library function. It has the same format string syntax
but a completely different implementation path.

### 1.3 What kprintf must support

For a minimal but useful kernel printf, we need these format
specifiers:

| Specifier | Meaning                        | Example output    |
|-----------|--------------------------------|-------------------|
| `%d`      | Signed decimal integer         | `-42`, `100`      |
| `%u`      | Unsigned decimal integer       | `42`, `100`       |
| `%x`      | Unsigned hexadecimal (lowercase) | `2a`, `ff`      |
| `%p`      | Pointer (hex with `0x` prefix) | `0x80001000`      |
| `%s`      | Null-terminated string         | `hello`           |
| `%c`      | Single character               | `A`               |
| `%%`      | Literal percent sign           | `%`               |

This is a small subset of what the full C `printf` supports (no
floating point, no width/precision, no padding flags), but it covers
every common kernel debugging need. Linux's `printk` started with a
similarly minimal set in the early days.

### 1.4 The building blocks

Implementing kprintf requires three distinct pieces:

```
 kprintf("hart %d: addr 0x%x\n", id, addr)
    |
    v
 +-----------------------+
 | 1. Variadic arguments |    How does C pass an unknown
 |    (stdarg.h)         |    number of arguments?
 +-----------------------+
    |
    v
 +-----------------------+
 | 2. Format string      |    Walk the string, find '%'
 |    parsing            |    specifiers, dispatch
 +-----------------------+
    |
    v
 +-----------------------+
 | 3. Value conversion   |    Turn integers into digit
 |    (int -> string)    |    characters in base 10/16
 +-----------------------+
    |
    v
 uart_putc() for each character
```

We will cover each piece in its own section.

---

## Part 2: Variadic Functions — How C Passes "..." Arguments

### 2.1 The problem

Look at the prototype we want:

```c
void kprintf(const char *fmt, ...);
```

The `...` (ellipsis) means "zero or more additional arguments of any
type." The compiler does not know how many arguments the caller will
pass or what types they will be. So how does the function access them?

### 2.2 Calling conventions and the stack/registers

When a function is called on RISC-V, the first eight integer
arguments go in registers `a0` through `a7`. Any additional arguments
go on the stack. For `kprintf("x=%d y=%d", x, y)`:

```
 a0 = pointer to "x=%d y=%d"    (fmt)
 a1 = value of x                (first variadic arg)
 a2 = value of y                (second variadic arg)
```

The key insight: the compiler knows where it *placed* the arguments
(it generated the caller's code), but the callee — `kprintf` — does
not know how many there are or what types they have. The format string
is the **contract** between caller and callee: `%d` means "the next
argument is an int," `%s` means "the next argument is a `char *`,"
and so on.

If the format string says `%d` but the caller passed a string pointer,
you get garbage. There is no runtime type checking — this is C.
Modern compilers can warn about mismatches if you annotate the
function with `__attribute__((format(printf, ...)))`, but the language
itself does not enforce it.

### 2.3 stdarg.h — the portable interface

C provides a standard header `<stdarg.h>` with four macros for
accessing variadic arguments:

| Macro                     | Purpose                                       |
|---------------------------|-----------------------------------------------|
| `va_list ap`              | Declare an argument pointer that tracks the current position in the argument list |
| `va_start(ap, last_named)`| Initialize `ap` to point just past the last named parameter |
| `va_arg(ap, type)`        | Retrieve the next argument as `type`, advance `ap` |
| `va_end(ap)`              | Clean up (required by the standard, usually a no-op on RISC-V) |

The name `ap` stands for **argument pointer** — it points to the
current position in the variadic argument list. You will see this
convention everywhere: the Linux kernel, glibc, xv6, and our kprintf
all use `ap` as the `va_list` variable name.

A simple example:

```c
#include <stdarg.h>

int sum(int count, ...) {
    va_list ap;
    va_start(ap, count);   /* ap now points past 'count' */

    int total = 0;
    for (int i = 0; i < count; i++)
        total += va_arg(ap, int);  /* fetch next int, advance */

    va_end(ap);
    return total;
}

/* sum(3, 10, 20, 30) returns 60 */
```

### 2.4 Where does stdarg.h come from?

You might wonder: we are building a freestanding kernel with no C
library. Can we use `<stdarg.h>`?

Yes! `<stdarg.h>` is special. It is a **compiler-provided** header,
not a library header. GCC and Clang both ship their own `stdarg.h`
that works without any library. When you compile with `-ffreestanding`,
you lose `<stdio.h>`, `<stdlib.h>`, and friends, but you keep:

- `<stdarg.h>` — variadic argument access
- `<stddef.h>` — `size_t`, `NULL`, `offsetof`
- `<stdint.h>` — fixed-width integer types
- `<stdbool.h>` — `bool`, `true`, `false`
- `<float.h>` and `<limits.h>` — numeric limits

These headers are all implemented with **compiler builtins** and
require zero library support. They are explicitly permitted in
freestanding C by the standard (C11 section 4, paragraph 6).

What does "compiler builtins" mean concretely? When you write
`va_start(ap, fmt)`, it looks like a function call, but the compiler
never generates a `call` instruction for it. The header defines it as
a macro that expands to a **compiler intrinsic**:

```c
/* What GCC/Clang's stdarg.h actually contains (simplified): */
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)
```

The `__builtin_*` names are special — the compiler recognizes them
during compilation and emits the correct machine instructions inline,
just like it knows how to compile `+` or `=`. There is no function
body anywhere in a `.c` or `.a` library file. The compiler *is* the
implementation.

Compare with `printf()` from `<stdio.h>`: that is a real function
with a real implementation inside glibc (thousands of lines of C). If
you do not link glibc, you do not have `printf()`. But `va_start` has
no library code to link — the compiler emits the instructions
directly.

This is also why `va_arg` needs the type parameter — it is not
runtime type information. It tells the compiler how many bytes to read
and how to interpret them. The compiler can do this because *it chose
where to put the arguments in the first place* when it compiled the
caller. The compiler is on both sides of the contract.

### 2.5 How va_arg works under the hood (RISC-V)

On RISC-V with the standard calling convention, `va_list` is
essentially a pointer into the saved register area or the stack.
`va_start` sets it to the address of the first variadic argument,
and `va_arg` reads the value at that address and advances the pointer
by the size of the requested type.

```
Before va_start:
    ap = ???

After va_start(ap, fmt):
    ap ---> [ arg1 ][ arg2 ][ arg3 ] ...
             a1      a2      a3

After va_arg(ap, int):
    ap -----------> [ arg2 ][ arg3 ] ...
    returns value of arg1
```

The exact layout depends on the ABI. On RV64, every argument slot is
8 bytes (64 bits), even for `int` (which is 32 bits). The unused
upper bits are either sign-extended or zero-extended depending on the
type. This is why `va_arg(ap, int)` works correctly — the compiler
knows to read 8 bytes from the slot and truncate or extend as needed.

### 2.6 Common pitfalls

**Pitfall 1: Wrong type in va_arg.** If the format string says `%x`
and you call `va_arg(ap, char *)`, you get undefined behavior. The
format string is the only source of truth.

**Pitfall 2: Integer promotion.** C promotes `char` and `short`
arguments to `int` when passed through `...`. So even if the caller
writes `kprintf("%c", 'A')`, the `'A'` arrives as an `int`. You
must use `va_arg(ap, int)` for `%c`, not `va_arg(ap, char)`.

**Pitfall 3: Forgetting va_end.** On most architectures it is a
no-op, but the standard requires it and some static analyzers will
flag the omission.

---

## Part 3: Parsing the Format String

### 3.1 The format string is a tiny program

Think of a format string like `"hart %d: addr 0x%x\n"` as a small
program. The printf implementation is an **interpreter** that walks
through it character by character:

- **Literal characters** (everything that is not `%`) are printed
  directly.
- **`%` followed by a specifier** means "fetch the next argument and
  print it in the specified format."
- **`%%`** is an escape: print a single `%`.

### 3.2 The parsing loop

The core of any printf implementation is a loop that looks like this:

```
for each character c in fmt:
    if c != '%':
        output c
        continue

    advance to next character (the specifier)
    switch on specifier:
        'd' -> print signed decimal integer
        'u' -> print unsigned decimal integer
        'x' -> print unsigned hex integer
        'p' -> print pointer
        's' -> print string
        'c' -> print character
        '%' -> output '%'
        else -> output '%', output the unknown specifier
```

The `else` case (unknown specifier) is a safety net. If someone
writes `kprintf("%q", x)`, we print `%q` literally rather than
silently ignoring it or crashing. This makes format string bugs
visible.

### 3.3 A walkthrough example

Let us trace `kprintf("hart %d: 0x%x\n", 2, 0x80001000)`:

```
 Character   Action                       Output so far
 ---------   ------                       -------------
 'h'         literal                      h
 'a'         literal                      ha
 'r'         literal                      har
 't'         literal                      hart
 ' '         literal                      hart_
 '%'         start specifier...
 'd'         va_arg(ap, int) -> 2
             print_int(2, 10)             hart 2
 ':'         literal                      hart 2:
 ' '         literal                      hart 2:_
 '0'         literal                      hart 2: 0
 'x'         literal                      hart 2: 0x
 '%'         start specifier...
 'x'         va_arg(ap, unsigned int)
             -> 0x80001000
             print_uint(0x80001000, 16)   hart 2: 0x80001000
 '\n'        literal                      hart 2: 0x80001000\n

 (underscores represent spaces for visibility)
```

Notice that the `0x` before `%x` is just literal text in the format
string. The `%x` specifier itself only prints the hex digits. This is
a common pattern: the caller provides the prefix in the format string.

The `%p` specifier is different — it adds the `0x` prefix
automatically, so `kprintf("%p", ptr)` prints `0x80001000` without
the caller needing to write `"0x%x"`.

---

## Part 4: Integer-to-String Conversion

### 4.1 The core problem

When kprintf encounters `%d` and gets the value `42`, it must output
the characters `'4'` and `'2'`. When it encounters `%x` and gets
`255`, it must output `'f'` and `'f'`. This is **integer-to-string
conversion in a given base**.

The algorithm is the same regardless of base:

1. **Divide** the number by the base. The **remainder** is the
   lowest digit.
2. **Repeat** with the quotient until it becomes zero.
3. The digits come out in **reverse order** (least significant first).

Example with 42 in base 10:

```
 42 / 10 = 4, remainder 2   -> digit '2'
  4 / 10 = 0, remainder 4   -> digit '4'

 Digits collected: '2', '4'
 Reversed: "42"
```

Example with 255 in base 16:

```
 255 / 16 = 15, remainder 15  -> digit 'f'
  15 / 16 =  0, remainder 15  -> digit 'f'

 Digits collected: 'f', 'f'
 Reversed: "ff"
```

> **Shift instead of divide?** For power-of-2 bases, division and
> modulo can be replaced with bit shifts and masks: `val % 16` is the
> same as `val & 0xF`, and `val / 16` is the same as `val >> 4`.
> Bitwise operations are cheaper than division on most hardware. In
> practice though, the compiler already knows this trick — if you write
> `val % 16` and the base is a compile-time constant, GCC/Clang will
> emit shift and mask instructions automatically. So we write the
> readable version (`%` and `/`) and get the fast version for free.
> xv6's `printptr` (Part 5) uses explicit shifts as a stylistic choice.

### 4.2 Mapping remainders to digit characters

For base 10, the remainder is always 0-9, and we convert it with
`'0' + remainder`. For base 16, remainders 0-9 use `'0' + remainder`
and remainders 10-15 use `'a' + (remainder - 10)`.

A common approach is to use a lookup table:

```c
static const char digits[] = "0123456789abcdef";
/* digits[0]  = '0'
   digits[9]  = '9'
   digits[10] = 'a'
   digits[15] = 'f' */
```

Then the conversion is simply `digits[remainder]`, regardless of base.

### 4.3 Two approaches: buffer vs. recursion

Since the digits come out in reverse order, we need a strategy to
print them in the correct order. There are two classic approaches:

**Approach A: Buffer and reverse.** Collect digits into a temporary
array, then print them backwards:

```c
void print_int_buf(unsigned long val, int base) {
    char buf[20];  /* enough for 2^64 in decimal */
    int i = 0;

    if (val == 0) {
        uart_putc('0');
        return;
    }

    while (val > 0) {
        buf[i++] = digits[val % base];
        val /= base;
    }

    while (i > 0)
        uart_putc(buf[--i]);
}
```

**Approach B: Recursion.** Print higher digits first by recursing
before printing the current digit:

```c
void print_int_rec(unsigned long val, int base) {
    if (val >= base)
        print_int_rec(val / base, base);
    uart_putc(digits[val % base]);
}
```

The recursive approach is elegant and uses no explicit buffer — the
call stack *is* the buffer. But it uses stack space proportional to
the number of digits (at most 20 frames for a 64-bit decimal number,
which is fine for kernel use).

> **Can we `inline` the recursion away?** No — `inline` and recursion
> are fundamentally incompatible. Inlining means "paste the function
> body at the call site," but a recursive function calls *itself*, so
> the compiler would need to paste infinitely. The compiler will simply
> ignore the `inline` hint on a recursive function.

Let us trace the recursion for `print_int_rec(42, 10)`:

```
 print_int_rec(42, 10)
   42 >= 10, so recurse with 42/10 = 4
     print_int_rec(4, 10)
       4 < 10, base case
       uart_putc(digits[4 % 10]) -> uart_putc('4')
     uart_putc(digits[42 % 10]) -> uart_putc('2')

 Output: "42"  (correct order!)
```

The xv6 kernel uses the buffer approach. We will also use a buffer
in our implementation — it is straightforward and avoids recursive
function calls.

### 4.4 Handling signed numbers

For `%d` (signed decimal), we need to handle negative values. The
standard approach:

1. If the value is negative, print `'-'` and negate the value.
2. Then convert the (now positive) value as unsigned.

This is what most kernel printf implementations do (including xv6):
print the minus sign, then pass the negated value to the unsigned
conversion function. Simple and readable.

But there is a subtle edge case: `INT_MIN`. For a 32-bit `int`,
`INT_MIN` is `-2147483648` and `INT_MAX` is `2147483647`. Negating
`INT_MIN` overflows — the positive value `2147483648` does not fit
in a signed 32-bit integer!

On two's complement machines (which is all modern hardware, and
mandated by C23), the overflow wraps and the subsequent cast to
unsigned produces the correct result `2147483648`. We will use this
simple pattern. If you need a portable approach that avoids the
overflow entirely, you can negate in the unsigned domain:

```c
/* Portable approach — avoids signed overflow entirely: */
uval = ~((unsigned int)val) + 1;  /* two's complement negate */
```

But for our kernel targeting RISC-V with a modern compiler, the
simple `(unsigned long)(-val)` is fine.

> **Why does `-INT_MIN` wrap to the right answer?** Let us trace it
> at the bit level using 8-bit integers (same principle for 32/64-bit).
> `INT_MIN` for 8-bit signed is `-128`, stored as `1000 0000`.
> Negation in two's complement is "invert all bits, then add 1":
>
> ```
>  expression           bits         signed    unsigned
>  ---                  ---------    ------    --------
>  INT_MIN              1000 0000    -128
>  invert               0111 1111    +127
>  add 1 (overflow!)    1000 0000    -128
>  cast to unsigned     1000 0000              128       <- correct!
> ```
>
> Inverting `1000 0000` gives `0111 1111` (127). Adding 1 to 127
> should give 128, but 128 does not fit in 8 signed bits — the result
> wraps back to `1000 0000`, which is `-128` again. So `-(-128) = -128`:
> this is the only signed value that negates to itself.
>
> But then the cast to unsigned reinterprets the same bit pattern
> `1000 0000` as `128` — exactly the positive magnitude we wanted. No
> bits change, just the interpretation.
>
> Before C23, the "add 1 (overflow!)" step was undefined behavior —
> the standard allowed the compiler to do anything. After C23, the
> standard mandates two's complement wrapping: the 8-bit result is
> `1000 0000`, and that is the defined answer.

### 4.5 Handling 64-bit values

Pointers on RV64 are 64 bits. When we print `%p`, we need to format
a full 64-bit hex value. Our conversion function should work with
`unsigned long` (which is 64 bits on RV64) so it handles both 32-bit
`%x`/`%d` values (promoted to 64-bit) and native 64-bit pointers.

For `%p`, we also want to:
- Print the `0x` prefix automatically.
- Always print the full address (no leading-zero suppression is fine
  for our minimal implementation, but the standard `%p` format varies
  by system).

---

## Part 5: How xv6 Does It

Let us look at the relevant parts of xv6's `kernel/printf.c`. This
is the complete formatted-output system for xv6.

### 5.1 The digit table and printint

```c
/* xv6: kernel/printf.c (adapted — original uses int/unsigned int,
   shown here with long long to illustrate the 64-bit pattern) */
static char digits[] = "0123456789abcdef";

static void
printint(long long xx, int base, int sign)
{
    char buf[16];
    int i;
    unsigned long long x;

    if (sign && (sign = (xx < 0)))
        x = -xx;
    else
        x = xx;

    i = 0;
    do {
        buf[i++] = digits[x % base];
        x /= base;
    } while (x != 0);

    if (sign)
        buf[i++] = '-';

    while (--i >= 0)
        consputc(buf[i]);
}
```

Key observations:

- **`sign` parameter:** Controls whether to treat the value as signed.
  `%d` passes `sign=1`, `%x` passes `sign=0`.
- **do-while loop:** Ensures that the value `0` prints as `"0"` (the
  loop body always executes at least once).
- **Buffer approach:** Digits are collected into `buf[]` in reverse
  order, then printed front-to-back with the `while(--i >= 0)` loop.
- **The minus sign** is appended to the buffer (after all digits) so
  it ends up at the front when printed in reverse order. Clever.
- **`consputc`** is xv6's character output function — it writes to
  the console UART. Our equivalent is `uart_putc`.

### 5.2 printptr

```c
/* xv6: kernel/printf.c */
static void
printptr(uint64 x)
{
    int i;
    consputc('0');
    consputc('x');
    for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
        consputc(digits[x >> (sizeof(uint64) * 8 - 4)]);
}
```

This is a different approach from `printint`. Instead of dividing
repeatedly, it extracts hex digits from the most significant nibble
(4 bits) first, shifting left each iteration. This always prints
exactly 16 hex digits (with leading zeros), giving a fixed-width
output like `0x0000000080001000`.

The expression `x >> (sizeof(uint64) * 8 - 4)` shifts the top 4 bits
down to positions 3:0, giving a value 0-15 that indexes into
`digits[]`. Then `x <<= 4` shifts the next nibble into the top
position.

```
 x = 0x0000000080001000

 Iteration 0: top nibble = 0x0 -> '0',  shift left
 Iteration 1: top nibble = 0x0 -> '0',  shift left
 ...
 Iteration 8: top nibble = 0x8 -> '8',  shift left
 Iteration 9: top nibble = 0x0 -> '0',  shift left
 ...
 Iteration 12: top nibble = 0x1 -> '1', shift left
 Iteration 13-15: top nibble = 0x0 -> '0'

 Output: 0x0000000080001000
```

### 5.3 The main printf function

```c
/* xv6: kernel/printf.c (simplified) */
void
printf(char *fmt, ...)
{
    va_list ap;
    int i, c;
    char *s;

    if (fmt == 0)
        panic("null fmt");

    va_start(ap, fmt);
    for (i = 0; (c = fmt[i] & 0xff) != 0; i++) {
        if (c != '%') {
            consputc(c);
            continue;
        }
        c = fmt[++i] & 0xff;
        if (c == 0)
            break;
        switch (c) {
        case 'd':
            printint(va_arg(ap, int), 10, 1);
            break;
        case 'x':
            printint(va_arg(ap, int), 16, 0);
            break;
        case 'p':
            printptr(va_arg(ap, uint64));
            break;
        case 's':
            if ((s = va_arg(ap, char*)) == 0)
                s = "(null)";
            for (; *s; s++)
                consputc(*s);
            break;
        case '%':
            consputc('%');
            break;
        default:
            /* Print unknown %verb literally. */
            consputc('%');
            consputc(c);
            break;
        }
    }
    va_end(ap);
}
```

The structure is exactly what we described in Part 3: walk the format
string, dispatch on the specifier character. Notice:

- **`& 0xff`**: Ensures the character is treated as unsigned (prevents
  sign extension if `char` is signed on the platform).
- **NULL string check**: If `%s` receives a NULL pointer, it prints
  `"(null)"` instead of crashing. Defensive programming.
- **`%x` uses `va_arg(ap, int)`**: xv6 fetches hex values as signed
  `int`. This works because `printint` with `sign=0` casts to unsigned
  internally. Our implementation will use `va_arg(ap, unsigned int)`
  and call `print_uint` directly, which is clearer.
- **No `%c`**: xv6 omits `%c`. We will add it because it is trivially
  easy and useful for debugging.
- **No `%u`**: xv6 omits unsigned decimal. We will add it as well.

### 5.4 Locking in xv6's printf

The full xv6 printf wraps the output loop in a spinlock:

```c
void
printf(char *fmt, ...)
{
    locking = pr.locking;
    if (locking)
        acquire(&pr.lock);
    /* ... formatting loop ... */
    if (locking)
        release(&pr.lock);
}
```

This prevents two CPUs from interleaving their output into garbage.
If CPU 0 prints `"hello\n"` and CPU 1 prints `"world\n"` at the same
time, without the lock you might see `"hewollrold\n\n"`.

We will skip locking for now because bobchouOS is single-hart in
Phase 1. When we add multiprocessor support in Phase 2, we will add a
spinlock to kprintf.

### 5.5 panic()

xv6 also defines `panic()` in printf.c:

```c
void
panic(char *s)
{
    printf("panic: ");
    printf(s);
    printf("\n");
    panicked = 1;  /* freeze other CPUs' output */
    for (;;)
        ;
}
```

`panic()` is the kernel's "something went terribly wrong" function.
It prints a message and halts. We will implement a simple version of
`panic()` alongside kprintf.

---

## Part 6: Our kprintf Design

### 6.1 Public interface

We will provide two functions in `kprintf.h`:

```c
void kprintf(const char *fmt, ...);
void panic(const char *fmt, ...);
```

Notice that our `panic()` takes a format string, unlike xv6's which
takes a plain string. This is more convenient:

```c
/* xv6 style — awkward for variable data: */
printf("panic: unexpected trap %d", trapno);
panic("");

/* Our style — one call: */
panic("unexpected trap %d", trapno);
```

### 6.2 File organization

```
 kernel/
 ├── include/
 │   └── kprintf.h          <-- Public interface
 └── lib/
     └── kprintf.c          <-- Implementation
```

We put kprintf in `kernel/lib/` because it is a utility function,
not a driver. The `lib/` directory is for general-purpose kernel
library code (string functions, formatted output, etc.).

### 6.3 Internal helper functions

The implementation has two internal (static) helpers:

| Function      | Purpose                                                   |
|---------------|-----------------------------------------------------------|
| `print_uint`  | Convert unsigned integer to string in base 10 or 16       |
| `print_int`   | Handle sign for `%d`, then call `print_uint`              |

And the digit lookup table:

```c
static const char digits[] = "0123456789abcdef";
```

### 6.4 print_uint — buffer approach

We will use the buffer-and-reverse approach (like xv6) rather than
recursion:

```c
static void
print_uint(unsigned long val, int base)
{
    char buf[20];  /* 2^64 - 1 = 18446744073709551615 -> 20 decimal digits */
    int i = 0;

    do {
        buf[i++] = digits[val % base];
        val /= base;
    } while (val != 0);

    while (--i >= 0)
        uart_putc(buf[i]);
}
```

Why a 20-element buffer? The largest 64-bit unsigned value is
`18446744073709551615` (`2^64 - 1`), which is 20 digits. Hex values need at most
16 digits (`ffffffffffffffff`). So 20 is sufficient for both bases.

### 6.5 print_int — handling the sign

We use `long` (not xv6's `long long`) because on RV64 both are 64
bits. `long` is the natural width of a register on our platform.

```c
static void
print_int(long val, int base)
{
    /* For non-decimal bases, treat as unsigned. */
    if (base != 10) {
        print_uint((unsigned long)val, base);
        return;
    }

    if (val < 0) {
        uart_putc('-');
        print_uint((unsigned long)(-val), base);
    } else {
        print_uint((unsigned long)val, base);
    }
}
```

### 6.6 Pointer formatting

For `%p`, we print the `0x` prefix and then format the address as
hex using `print_uint`. Unlike xv6's `printptr`, we will not
zero-pad to 16 digits — our simpler approach just prints the
significant hex digits. This keeps the implementation minimal:

```c
case 'p':
    uart_putc('0');
    uart_putc('x');
    print_uint(va_arg(ap, unsigned long), 16);
    break;
```

### 6.7 Complete kprintf flow

Here is the complete control flow for a kprintf call:

```
 kprintf("boot: hart %d at %p\n", 0, 0x80000000)
   |
   v
 va_start(ap, fmt)
   |
   v
 Walk fmt character by character:
   |
   'b','o','o','t',':',' '  -->  uart_putc() each
   |
   '%' found, next char = 'd'
   |  va_arg(ap, int) = 0
   |  print_int(0, 10)
   |    print_uint(0, 10)
   |      buf[0] = '0', val = 0, loop ends
   |      uart_putc('0')
   |
   ' ','a','t',' '  -->  uart_putc() each
   |
   '%' found, next char = 'p'
   |  va_arg(ap, unsigned long) = 0x80000000
   |  uart_putc('0'), uart_putc('x')
   |  print_uint(0x80000000, 16)
   |    buf = ['0','0','0','0','0','0','0','8']
   |    print reversed: "80000000"
   |
   '\n'  -->  uart_putc('\n')
   |
   v
 va_end(ap)

 Final output: "boot: hart 0 at 0x80000000\n"
```

### 6.8 panic()

A first attempt at panic might look like this:

```c
void
panic(const char *fmt, ...)
{
    kprintf("PANIC: ");
    kprintf(fmt, ...);   /* ERROR: can't forward ... arguments! */
    kprintf("\n");
    for (;;)
        ;
}
```

But this does not compile. Here is the subtlety: we cannot simply call `kprintf(fmt, ...)`
and forward the variadic arguments. C does not allow forwarding `...`
arguments to another variadic function. Instead, panic needs its own
format-string parsing loop, or we factor out the core formatting into
a helper that takes a `va_list`.

The clean solution is to have an internal function:

```c
static void vprintfmt(const char *fmt, va_list ap);
```

Then both `kprintf` and `panic` call `vprintfmt`:

```c
void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintfmt(fmt, ap);
    va_end(ap);
}

void panic(const char *fmt, ...) {
    va_list ap;
    kprintf("PANIC: ");
    va_start(ap, fmt);
    vprintfmt(fmt, ap);
    va_end(ap);
    kprintf("\n");
    for (;;)
        ;
}
```

This is a common pattern. Linux's kernel has `vprintk` for the same
reason. The `v` prefix conventionally means "takes a `va_list`
instead of `...`". We call ours `vprintfmt` (v + print + fmt) rather
than `vkprintf` because it is a static helper, not part of the public
API — the name emphasizes what it does (format and print) rather than
mirroring the public function name.

---

## Part 7: Putting It All Together

### 7.1 How main.c changes

With kprintf available, our kernel entry point becomes much more
expressive:

```c
#include "kprintf.h"

void
kmain(void) {
    uart_init();
    kprintf("bobchouOS is booting...\n");
    kprintf("UART initialized at %p\n", (void *)0x10000000);

    /* We can now print diagnostics easily. */
    kprintf("kernel loaded: %d bytes\n", kernel_size);

    for (;;)
        ;
}
```

We also remove the hand-written `uart_puts()` from `main.c` — kprintf
replaces it entirely.

### 7.2 Include path

`kprintf.h` lives in `kernel/include/`, which is already on the
include path (we set `-I kernel/include` in the Makefile). So the
include is simply:

```c
#include "kprintf.h"
```

### 7.3 Makefile changes

We need to add `kernel/lib/kprintf.o` to the list of object files
that get linked into the kernel. The change is one line in the
Makefile's `OBJS` variable.

### 7.4 Complete call chain

```
 kmain()
   |
   +-- uart_init()           [drivers/uart.c]
   |
   +-- kprintf("...", ...)   [lib/kprintf.c]
         |
         +-- vprintfmt()     [lib/kprintf.c, static]
               |
               +-- uart_putc()    for literal chars
               +-- print_int()    for %d
               +-- print_uint()   for %u, %x, %p
               +-- uart_putc()    for %c
               +-- uart_putc()    loop for %s
```

### 7.5 Testing kprintf

After implementing kprintf, we can verify it works by printing
various values from `kmain()`:

```c
kprintf("decimal: %d %d %d\n", 0, 42, -1);
kprintf("hex: %x %x\n", 255, 0xDEADBEEF);
kprintf("pointer: %p\n", (void *)0x80001000);
kprintf("string: %s\n", "hello");
kprintf("char: %c\n", 'A');
kprintf("percent: %%\n");
kprintf("unsigned: %u\n", 4294967295u);
```

Expected output:

```
decimal: 0 42 -1
hex: ff deadbeef
pointer: 0x80001000
string: hello
char: A
percent: %
unsigned: 4294967295
```

If all these match, kprintf is working correctly.

---

## Part 8: Looking Ahead — From kprintf to a Real Console

### 8.1 What kprintf cannot do yet

Our Phase 1 kprintf is deliberately simple. Here is what it lacks
and when we will add it:

| Missing feature        | Why it matters             | When we add it      |
|------------------------|---------------------------|---------------------|
| Spinlock protection    | Multi-hart output interleaves | Phase 2 (spinlocks) |
| Width/padding (e.g. `%08x`) | Aligned columnar output | Optional/never     |
| `%ld`, `%lx`          | Explicit 64-bit specifiers | Can add if needed   |
| Floating point (`%f`)  | Not needed in kernel       | Never               |
| Output to buffer (`snprintf`) | Useful for string building | Phase 3+ |

### 8.2 From polling to interrupts (revisited)

Right now every `uart_putc()` call spins until the UART is ready.
When we add interrupt-driven UART in Phase 2, the output path will
change:

```
 Phase 1 (now):
   kprintf -> uart_putc -> spin until THR empty -> write THR

 Phase 2 (later):
   kprintf -> uart_putc -> append to output buffer -> return immediately
   (UART interrupt fires when THR empties -> ISR writes next byte from buffer)
```

kprintf itself will not change — only `uart_putc()` will be
rewritten. This is the beauty of the abstraction: kprintf does not
care *how* characters reach the UART, only that `uart_putc()` sends
them.

### 8.3 panic() and debugging

With `panic()` in place, we can now assert invariants in our kernel
code:

```c
if (ptr == NULL)
    panic("null pointer in %s", __func__);
```

The `__func__` variable is a compiler-provided string containing the
current function name. The C standard (C11 section 6.4.2.2) defines
it as if the compiler inserts the following declaration immediately
after the opening brace of every function:

```c
static const char __func__[] = "kmain";
```

Whether the compiler actually inserts a static array or implements it
some other way (e.g., a direct pointer to a string in a read-only
data section) is an implementation detail — the observable behavior
must match.

C provides several such built-in identifiers that are useful for
diagnostics:

| Name           | Type            | Example value        | Standard |
|----------------|-----------------|----------------------|----------|
| `__func__`     | string variable | `"kmain"`            | C99      |
| `__FILE__`     | string macro    | `"kernel/main.c"`    | C89      |
| `__LINE__`     | integer macro   | `42`                 | C89      |
| `__DATE__`     | string macro    | `"Apr 11 2026"`      | C89      |
| `__TIME__`     | string macro    | `"14:30:00"`         | C89      |

The first three are the most useful for kernel debugging. Combined
with `panic()`, they give us a minimal assertion system:

```c
if (ptr == NULL)
    panic("null pointer in %s at %s:%d", __func__, __FILE__, __LINE__);
/* Output: PANIC: null pointer in kmain at kernel/main.c:42 */
```

This will be invaluable as the kernel grows.

> **Why is `__func__` a variable but `__FILE__` and `__LINE__` are
> macros?** It comes down to who knows what. `__FILE__` and `__LINE__`
> are handled by the **preprocessor**, which runs *before* compilation.
> The preprocessor opens files and counts newlines — filenames and line
> numbers are part of its own bookkeeping, so it simply substitutes
> them as literal values:
>
> ```c
> /* You write (line 42 of kernel/main.c): */
> panic("error at %s:%d", __FILE__, __LINE__);
>
> /* After preprocessing: */
> panic("error at %s:%d", "kernel/main.c", 42);
> ```
>
> By the time the compiler sees the code, `__FILE__` and `__LINE__`
> are already gone — replaced with a string and an integer. Zero
> runtime cost.
>
> But function names are different. The preprocessor is a dumb text
> processor — it reads tokens, expands `#define`s, and handles
> `#include`. It does not understand C syntax. When it sees `void
> kmain(void) {`, it has no idea that `kmain` is a function name. It
> does not parse braces, declarations, or scopes. Only the full C
> compiler understands function boundaries, so `__func__` must be
> handled by the compiler, which is why the standard defines it as
> an implicit local variable rather than a preprocessor macro.

---

## What's Next

You now understand variadic functions, format string parsing, and
integer-to-string conversion — everything needed to build a kernel
printf. Next steps for this round:

1. **Skeleton files** — we create `kernel/include/kprintf.h`
   (complete) and `kernel/lib/kprintf.c` (with TODO markers to
   implement)
2. **Implement the TODOs** — the five functions:
   - `print_uint()` — buffer-and-reverse integer conversion
   - `print_int()` — signed integer handling
   - `vprintfmt()` — format string parsing loop
   - `kprintf()` — thin wrapper around vprintfmt
   - `panic()` — print prefix, call vprintfmt, halt
3. **Update `main.c` and Makefile** — to use `kprintf()` instead
   of the hand-written `uart_puts()`
4. **Verify** — `make run` should print formatted output with
   `%d`, `%u`, `%x`, `%p`, `%s`, `%c`, and `%%` all working

---

## Quick Reference

### Format specifiers

| Specifier | Argument type       | va_arg type       | Output            |
|-----------|---------------------|-------------------|-------------------|
| `%d`      | signed integer      | `int`             | Decimal with sign |
| `%u`      | unsigned integer    | `unsigned int`    | Decimal, unsigned |
| `%x`      | unsigned integer    | `unsigned int`    | Hex, no prefix    |
| `%p`      | pointer             | `unsigned long`   | Hex with `0x`     |
| `%s`      | string              | `char *`          | Characters until NUL |
| `%c`      | character           | `int` (promoted)  | Single character  |
| `%%`      | (none)              | (none)            | Literal `%`       |

### stdarg.h macros

| Macro                      | Purpose                                   |
|----------------------------|-------------------------------------------|
| `va_list ap`               | Declare argument-list tracker              |
| `va_start(ap, last_named)` | Initialize to first variadic argument      |
| `va_arg(ap, type)`         | Fetch next argument as `type`, advance     |
| `va_end(ap)`               | Clean up                                   |

### Integer conversion algorithm (buffer approach)

```
 1. do { buf[i++] = digits[val % base]; val /= base; } while (val);
 2. while (--i >= 0) uart_putc(buf[i]);
```

### Key constants

| Constant               | Value | Meaning                               |
|------------------------|-------|---------------------------------------|
| Max decimal digits (64-bit) | 20 | `2^64 - 1 = 18446744073709551615`    |
| Max hex digits (64-bit)     | 16 | `0xffffffffffffffff`                 |
| Buffer size            | 20    | Accommodates both bases               |

### Files we will create

| File                      | Contents                                   |
|---------------------------|--------------------------------------------|
| `kernel/include/kprintf.h`| `kprintf()` and `panic()` prototypes       |
| `kernel/lib/kprintf.c`    | Full implementation with helpers           |

### bobchouOS vs. xv6 comparison

| Aspect            | xv6 `printf()`            | bobchouOS `kprintf()`        |
|-------------------|---------------------------|------------------------------|
| Name              | `printf`                  | `kprintf`                    |
| Output function   | `consputc()`              | `uart_putc()`                |
| Locking           | Spinlock (`pr.lock`)      | None (Phase 1, single hart)  |
| `%d`              | Yes                       | Yes                          |
| `%u`              | No                        | Yes                          |
| `%x`              | Yes                       | Yes                          |
| `%p`              | Yes (zero-padded 16 hex)  | Yes (no zero-padding)        |
| `%s`              | Yes (NULL-safe)           | Yes (NULL-safe)              |
| `%c`              | No                        | Yes                          |
| `%%`              | Yes                       | Yes                          |
| `panic()`         | Plain string only         | Format string                |
| va_list helper    | No (inline in printf)     | Yes (`vprintfmt`)            |
