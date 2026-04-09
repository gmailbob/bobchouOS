# Lecture 0-3: C Language Recap

> **Scope and intent**
>
> This is a focused refresher on the C features that matter for OS
> development, not a comprehensive language tutorial. We assume you've
> written C (or C++) before and know the basics — variables, functions,
> loops, `if`/`else`. The goal is to sharpen the specific skills you'll
> use daily in kernel code: pointers, structs, bitwise operations, the
> preprocessor, and the patterns that glue them together.
>
> Three topics that often surprise developers coming from higher-level
> languages get dedicated sections: the C build model (how compilation
> and linking actually work), undefined behavior (the invisible contract
> the compiler exploits), and the "no standard library" reality of
> bare-metal C.

---

## Part 1: C Without an OS — What Changes

In Python or Java, you take for granted:

```python
print("Hello")          # just works
x = [1, 2, 3]           # memory allocated behind the scenes
import json              # modules loaded from filesystem
```

```java
System.out.println("Hello");        // just works
int[] x = new int[]{1, 2, 3};      // memory allocated and managed by GC
import java.util.List;              // modules loaded by classloader
```

All of these rely on an operating system underneath. `print` calls
`write()`, which is a system call. List allocation calls `malloc()`,
which asks the OS for memory. `import` reads files from disk.

In bare-metal C, **there is no OS below you — you are the OS.** This
means:

| What you lose | Why | What you do instead |
|---------------|-----|---------------------|
| `printf` | It calls `write()` → syscall → kernel. You *are* the kernel. | Write directly to UART hardware (we did this in Lecture 0-1) |
| `malloc` / `free` | They rely on the OS to provide heap memory via `brk`/`mmap`. | Build your own allocator in Phase 3 (`kalloc`/`kfree`) |
| `#include <string.h>` | `strlen`, `memcpy`, etc. are part of libc, which assumes a hosted environment. | Write your own versions (we'll do this in the exercises) |
| `exit()` | Returns control to the OS. There's no OS to return to. | Spin forever with `wfi` (we did this in `entry.S`) |
| Stack setup | The OS sets up the stack before your `main()` runs. | You do it yourself in assembly (`la sp, stack_top`) |

The compiler flags we use reflect this:

```
-ffreestanding     Don't assume a hosted C environment exists
-nostdlib          Don't link the standard library or startup files
```

`-ffreestanding` tells the compiler: "don't assume the full standard
library is available." It still gives you a few **freestanding headers**
that don't require OS support:

| Header | What it provides |
|--------|-----------------|
| `<stdint.h>` | Exact-width types: `uint8_t`, `uint32_t`, `int64_t`, etc. |
| `<stddef.h>` | `size_t`, `NULL`, `offsetof` |
| `<stdbool.h>` | `bool`, `true`, `false` |
| `<stdarg.h>` | `va_list`, `va_start`, `va_arg`, `va_end` (for variadic functions like `kprintf`) |
| `<limits.h>` | `INT_MAX`, `UINT64_MAX`, etc. |

These are "freestanding" because they only define types and macros — no
function implementations, no OS interaction. Everything else (`stdio.h`,
`stdlib.h`, `string.h`, etc.) is off-limits.

In practice, xv6 defines its own type aliases (e.g., `typedef unsigned
long uint64;`) rather than using `<stdint.h>`. We'll follow the same
convention. But it's good to know that `<stdint.h>` *is* available if
you want it — it's not cheating.


---

## Part 2: The C Build Model

Python runs a `.py` file directly. Java compiles `.java` files to
bytecode and loads classes dynamically through a classloader. C works
differently from both, and understanding the build model prevents a
whole class of confusing errors.

### Separate compilation

C compiles each `.c` file **independently** into an object file (`.o`).
Each `.c` file is called a **translation unit** — the compiler processes
it in isolation, with no knowledge of what's in other `.c` files.

```
main.c  ──→ gcc ──→ main.o
uart.c  ──→ gcc ──→ uart.o
proc.c  ──→ gcc ──→ proc.o
                       │
                       ▼  linker (ld)
                    kernel.elf
```

(The command is called `ld`, which stands for **Link eDitor** — a name
inherited from 1970s Unix, like `cc` for "C compiler", `as` for
"assembler", and `ar` for "archiver." These two-letter names reflect an
era when disk space was precious and commands were kept short.)

When `main.c` calls `uart_putc('H')`, the compiler doesn't check that
`uart_putc` exists or that you're passing the right type. It trusts your
**declaration** (the function prototype) and emits a placeholder: "call
the function named `uart_putc`." The **linker** fills in the actual
address later by finding `uart_putc` in `uart.o`.

This is why C has header files. Headers aren't modules — they're just
text that gets copy-pasted by the preprocessor. They exist to share
declarations across translation units.

### Headers — the "copy-paste" model

```c
// uart.h — declarations (promises about what exists)
void uart_putc(char c);
void uart_puts(const char *s);

// uart.c — definitions (actual code)
#include "uart.h"
void uart_putc(char c) {
    *(volatile char *)0x10000000 = c;
}
void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

// main.c — uses uart functions
#include "uart.h"    // preprocessor literally copies uart.h's text here
void main(void) {
    uart_puts("Hello\n");
}
```

`#include "uart.h"` is a textual substitution — the preprocessor
replaces the `#include` line with the entire contents of `uart.h`.
After preprocessing, `main.c` looks like:

```c
void uart_putc(char c);
void uart_puts(const char *s);

void main(void) {
    uart_puts("Hello\n");
}
```

The compiler sees the declaration `void uart_puts(const char *s);` and
knows what arguments and return type to expect. It doesn't need the
actual function body — that's in `uart.o` and the linker handles it.

### Declaration vs definition

A **declaration** says "this thing exists and here's its type." A
**definition** provides the actual implementation or storage.

```c
// Declarations (can appear multiple times):
void uart_putc(char c);            // function declaration
extern int counter;                // variable declaration
struct proc;                       // forward declaration (incomplete type)

// Definitions (must appear exactly once):
void uart_putc(char c) { ... }    // function definition
int counter = 0;                   // variable definition
struct proc { int pid; ... };      // struct definition
```

The **one definition rule**: each symbol can be defined in exactly one
translation unit. If two `.c` files both define `void uart_putc(char c)
{ ... }`, the linker will error: "multiple definition of `uart_putc`."
But both can *declare* it (via `#include "uart.h"`) without conflict.

### Linkage — `static` vs `extern`

Linkage controls whether a symbol is visible across translation units:

```c
// In uart.c:

void uart_putc(char c) { ... }    // external linkage (default)
                                   // visible to linker → other files can call it

static void uart_delay(void) { ... }  // internal linkage
                                       // invisible to linker → private to uart.c

extern int global_flag;            // declaration of external variable
                                   // definition must be in some other .c file
```

**`static` at file scope** means "private to this file." This is
completely different from `static` inside a function (which means "this
variable persists across calls"). Same keyword, different meaning
depending on context — one of C's unfortunate overloads.

Both flavors create a variable that lives for the program's entire
lifetime and is invisible to the linker. The difference is **scope** —
who can name the variable:

File-level static — any function in this file can touch `count`:

```c
static int count = 0;
void foo(void) { count++; }
void bar(void) { count++; }    // same variable — both can access it
```

Function-level static — only `foo()` can touch `count`:

```c
void foo(void) {
    static int count = 0;
    count++;
}
void bar(void) {
    // count doesn't exist here — compiler error
}
```

You could always use file-level `static` instead of function-level
`static` — they're functionally equivalent. The function-level version
just communicates tighter intent: "this state belongs to `foo` alone."
In practice, xv6 almost exclusively uses file-level `static` — state
is declared at the top of the file where it's easy to find, not
hiding inside functions. We'll follow the same convention.

You'll also see `static` on functions (not just variables) to keep
helpers file-private, preventing name collisions and making it clear
what's part of the module's public interface vs internal implementation.

### How the linker resolves symbols

When the linker combines `.o` files into an ELF binary, it:

1. Collects all **defined** symbols (functions and global variables with
   bodies/initial values) from all `.o` files
2. Collects all **undefined** symbols (references to things not defined
   in the same `.o` file)
3. Matches each undefined reference to a definition
4. Errors if a symbol is defined more than once or referenced but never
   defined

**The linker only knows names, not types.** When the linker sees
`main.o` reference `uart_putc`, it searches other `.o` files for a
symbol with that name. If it finds one, it fills in the address. It
never checks that the function signature matches what `main.c` expected
— types are the compiler's concern, and the compiler only sees one
translation unit at a time.

This has two consequences:

**Consequence 1: `.c` files should include their own header.**

You might wonder why `uart.c` does `#include "uart.h"` when it already
has the actual function body. The include is a **self-check**: it lets
the compiler see both the declaration (from the header) and the
definition (in the same file) simultaneously. If they disagree — say
you changed the parameter from `char` to `int` in the `.c` but forgot
to update the header — the compiler catches it immediately:
"conflicting types for `uart_putc`." Without the include, the mismatch
would slip through to the linker, which doesn't check types, producing
a subtle runtime bug.

**Consequence 2: you can swap implementations by swapping `.o` files.**

Since the linker matches only by name, you can provide a different `.o`
file with the same symbol name but a completely different implementation.
For example, in tests you might replace the real `uart_putc` (which
writes to hardware) with a mock that logs to a buffer:

```c
// mock_uart.c — used in test builds instead of uart.c
static char log_buf[256];
static int log_pos = 0;

void uart_putc(char c) {
    log_buf[log_pos++] = c;    // capture output instead of hitting hardware
}
```

Compile `mock_uart.c` instead of `uart.c`, link with the same `main.o`,
and the linker is happy — it found a symbol named `uart_putc`, job
done. This is how mock testing works in C: swapping `.o` files at link
time. (If you've used Google Test with C++, this is the same concept —
link-time substitution of implementations.)

In Java, the classloader verifies method signatures at load time and
throws `NoSuchMethodError` if they don't match. C has no such safety
net — the header convention is all we have.

Common linker errors and what they mean:

```
undefined reference to `uart_putc'
  → you declared it but never defined it (forgot to compile uart.c?)

multiple definition of `uart_putc'
  → two .c files both have the function body (did you put code in a header?)

undefined reference to `main'
  → linker expects main() but you named it something else
```

If you put a **function definition** in a header file and include it
from two `.c` files, you get "multiple definition" because both `.o`
files contain the compiled function body. The fix is either:
- Put only declarations in headers, definitions in `.c` files
- Mark the function `static inline` (each `.c` gets its own private
  copy — standard pattern for tiny helpers, discussed in Part 11)

### Building C programs — the actual commands

Everything above was conceptual. Here's what you actually type.

**Case 1: Single-file program on a normal Linux machine.**

```bash
# hello.c is a self-contained program with main()
gcc hello.c -o hello
./hello
```

This does everything in one shot: preprocess → compile → assemble →
link. GCC calls the preprocessor, compiler, assembler, and linker
internally, creating a temporary `.o` file behind the scenes and
immediately linking it into the final executable. You never see the
`.o` file.

**Case 2: Multi-file program on a normal Linux machine.**

```bash
# Step 1: compile each .c file into a .o file (separately)
gcc -c main.c -o main.o        # preprocess + compile + assemble → main.o
gcc -c uart.c -o uart.o        # preprocess + compile + assemble → uart.o

# Step 2: link all .o files into one executable
gcc main.o uart.o -o program   # link → program
./program
```

The `-c` flag means "compile only, don't link." It produces an **object
file** (`.o`). The name comes from "object code" — 1960s terminology
for "the output of a compiler" (the code that is the *object* of the
compilation process). Nothing to do with object-oriented programming.
The `.o` extension is Unix convention, same era as `.c`, `.s`, `.a`.

An `.o` file contains:
- **Machine code** — real compiled instructions, ready to execute
- **Symbol table** — "I define `uart_putc` at offset 0x20, and I need
  someone to provide `main`"
- **Relocation entries** — "patch the address at offset 0x34 when you
  know where `main` ends up"

It's *almost* a runnable program — it has real instructions — but the
addresses aren't finalized. The linker takes multiple `.o` files,
assigns final addresses, patches all the placeholders, and produces the
executable.

> **Why relocations are needed**
>
> When the compiler processes `main.c` and sees `uart_puts("Hello\n")`,
> it needs to emit a `call uart_puts` instruction. But `call` encodes
> the **distance** from the current instruction to the target — and the
> compiler has no idea where `uart_puts` will end up. It's defined in
> `uart.o`, which hasn't been placed in memory yet. Even the string
> `"Hello\n"` gets an unknown address — it goes in `.rodata`, but the
> compiler doesn't know where `.rodata` will be.
>
> So the compiler emits the instruction with a **placeholder address**
> (zero) and leaves a relocation entry: "at offset 0x00, fill in the
> address of `uart_puts`."
>
> ```
> main.o contents (simplified):
>
> .text (machine code):
>   offset 0x00:  auipc ra, 0x0     ← first half of "call uart_puts" (address = 0)
>   offset 0x04:  jalr  ra, ra, 0x0 ← second half (also placeholder)
>
> .rela.text (relocation entries):
>   "At offset 0x00, patch in uart_puts using R_RISCV_CALL relocation"
> ```
>
> Then the linker runs. It places `uart_puts` at `0x80000100` and
> `main` at `0x80000200`. Now it calculates the distance (`-0x100`),
> and patches the `auipc`+`jalr` pair with that offset. The placeholder
> becomes a real, working function call.
>
> Without relocations, you'd need one of these worse alternatives:
> - **Fix all addresses at compile time** — every `.c` file needs to
>   know the final layout. Change one file → recompile everything. No
>   separate compilation.
> - **Resolve at runtime** — this is what Java's classloader does.
>   Works, but too slow for a kernel — you'd pay the lookup cost on
>   every call.
>
> Relocations give you the best of both: compile separately, link once,
> run at full speed with hardcoded addresses.

Why not just `gcc main.c uart.c -o program`? You can — GCC will compile
and link in one step. But in real projects you use `-c` for two reasons:

1. **Incremental builds.** If you change `uart.c`, you only recompile
   `uart.o` and re-link. Without `-c`, you'd recompile everything every
   time. On a project with hundreds of files, this matters.

2. **Makefiles automate this.** Make tracks which `.c` files changed
   and only rebuilds the affected `.o` files. This only works if
   compilation and linking are separate steps.

**Breaking down what GCC actually does:**

When you run `gcc -c main.c -o main.o`, four phases happen inside:

```
main.c
  │  1. Preprocessor (cpp)
  │     - expands #include (copy-paste headers)
  │     - expands #define macros
  │     - processes #ifdef conditionals
  ▼
main.i                    (preprocessed C — pure C with no directives)
  │  2. Compiler (cc1)
  │     - parses C, checks types
  │     - generates assembly
  ▼
main.s                    (assembly text)
  │  3. Assembler (as)
  │     - converts assembly to machine code
  ▼
main.o                    (object file — machine code + symbol table)
```

You can stop at any intermediate stage:

```bash
gcc -E main.c -o main.i       # preprocess only — see what #include expands to
gcc -S main.c -o main.s       # compile to assembly — see what the compiler generates
gcc -c main.c -o main.o       # compile + assemble — produce object file
```

`-E` is particularly useful for debugging macro problems — you can see
exactly what the preprocessor produced before the compiler saw it.

**Case 3: Our project — bare-metal cross-compilation.**

Same steps, but with a cross-compiler and extra flags:

```bash
# Step 1: compile each file
riscv-none-elf-gcc -c -march=rv64imac -mabi=lp64 \
    -ffreestanding -nostdlib -mcmodel=medany -Wall -O2 -g \
    main.c -o main.o

riscv-none-elf-gcc -c -march=rv64imac -mabi=lp64 \
    -ffreestanding -nostdlib -mcmodel=medany -Wall -O2 -g \
    uart.c -o uart.o

# Step 1b: assemble .S files (same compiler, same flags)
riscv-none-elf-gcc -c -march=rv64imac -mabi=lp64 -g \
    entry.S -o entry.o

# Step 2: link with a custom linker script
riscv-none-elf-ld -T linker.ld -o kernel.elf entry.o main.o uart.o
```

Compared to the normal Linux case, there are three differences:

| Normal Linux | Our project | Why |
|-------------|-------------|-----|
| `gcc` | `riscv-none-elf-gcc` | Cross-compile: host is ARM64, target is RISC-V |
| No special flags | `-ffreestanding -nostdlib` | No libc, no OS, no startup files |
| `gcc` links | `riscv-none-elf-ld -T linker.ld` | We control the memory layout ourselves |

On normal Linux, `gcc` handles linking internally — it calls `ld`
behind the scenes with the system's default linker script, which places
`.text` at a standard address, links libc, and adds a C runtime startup
(`crt0.o` that calls `main()`). We can't use any of that — we provide
our own linker script, our own `entry.S` (instead of `crt0`), and no
libc.

**The Makefile puts it all together.**

Nobody types these commands by hand. A `Makefile` automates it:

```makefile
CROSS = riscv-none-elf-
CC    = $(CROSS)gcc
LD    = $(CROSS)ld

CFLAGS  = -march=rv64imac -mabi=lp64 -ffreestanding -nostdlib \
          -mcmodel=medany -Wall -O2 -g
ASFLAGS = -march=rv64imac -mabi=lp64 -g

# Pattern rules: how to build any .o from a .c or .S
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(ASFLAGS) -c -o $@ $<

# Link all objects into the final ELF
kernel.elf: entry.o main.o uart.o linker.ld
	$(LD) -T linker.ld -o $@ entry.o main.o uart.o

# Convenience targets
qemu: kernel.elf
	qemu-system-riscv64 -machine virt -nographic -bios none -kernel $<

clean:
	rm -f *.o kernel.elf
```

`make` reads this file, checks which `.o` files are older than their
`.c` source, recompiles only those, then re-links. `$@` means "the
target" and `$<` means "the first prerequisite" — Makefile syntax you'll
see everywhere but don't need to memorize right now.

The Makefile in the Lecture 0-2 exercises directory follows this exact
pattern — you've already been using it with `make qemu`.

> **Build systems across languages**
>
> Make is not the only build tool — different ecosystems have their own:
>
> | Domain | Build system |
> |--------|-------------|
> | OS kernels, embedded, bare-metal | **Make** |
> | Large C++ projects (enterprise) | **CMake** |
> | Google C++ (internal) | Bazel |
> | Rust | Cargo (built-in) |
> | Go | `go build` (built-in) |
> | Java | Gradle / Maven |
> | Python | pip / setuptools |
> | JavaScript | npm / webpack |
>
> For C++ specifically, **CMake** is the de facto standard. It's not a
> build tool itself — it's a **build system generator** that reads a
> `CMakeLists.txt` file and produces a Makefile (or Ninja file, or
> Visual Studio project). CMake solves problems that hurt large C++
> projects: cross-platform builds, auto-detecting header dependencies,
> and finding third-party libraries. Major projects like LLVM, Qt, and
> OpenCV all use it.
>
> We use plain Make because it's the standard for OS projects (xv6 and
> Linux both use it), and because we *want* to see the raw compiler
> commands — CMake's abstraction layer hides exactly the things we're
> trying to learn. Newer languages (Rust, Go) ship with built-in build
> systems partly as a reaction to the C/C++ build tooling mess —
> decades of Make, Autotools, CMake, Meson, Bazel, each solving part
> of the problem.


---

## Part 3: Types and Sizes

### Fundamental types on RV64

C's integer types have **implementation-defined** sizes — `int` might
be 16, 32, or 64 bits depending on the platform. For OS code that
manipulates hardware registers and memory addresses, you need exact
control. Here are the sizes on our platform (RV64, LP64 ABI):

| C type | Size (bytes) | Size (bits) | Typical use |
|--------|-------------|-------------|-------------|
| `char` | 1 | 8 | Single byte, ASCII character |
| `short` | 2 | 16 | Rare in kernel code |
| `int` | 4 | 32 | Loop counters, small values |
| `long` | 8 | 64 | Addresses, sizes, register values |
| `long long` | 8 | 64 | Same as `long` on LP64 |
| pointer | 8 | 64 | All pointer types |

The "LP64" in our ABI (`-mabi=lp64`) stands for "Long and Pointers are
64-bit." Some platforms use ILP32 (Int, Long, Pointers all 32-bit) or
LLP64 (Long Long and Pointers 64-bit, but Long is 32-bit — Windows
does this). This is why you never assume `sizeof(long)` — it varies.

### Exact-width types

To avoid ambiguity, kernel code uses typedefs that make the size
explicit:

```c
// xv6's type definitions (from types.h):
typedef unsigned int   uint;
typedef unsigned short ushort;
typedef unsigned char  uchar;

typedef unsigned char  uint8;
typedef unsigned short uint16;
typedef unsigned int   uint32;
typedef unsigned long  uint64;
```

When you see `uint64` in xv6, you know it's **exactly** 8 bytes. No
guessing. We'll use these same typedefs.

The standard library equivalent is `<stdint.h>` which defines
`uint8_t`, `uint16_t`, `uint32_t`, `uint64_t`, and their signed
counterparts. xv6 uses its own names (without the `_t` suffix) to
avoid depending on any standard header, but they're the same concept.

> **C naming conventions**
>
> Unlike Java (where classes are `PascalCase`), C uses lowercase for
> almost everything. The conventions in kernel code:
>
> ```c
> // Types — lowercase
> int, char, struct proc, uint64
>
> // Macros/constants — ALL CAPS
> #define PGSIZE 4096
> #define NULL ((void *)0)
>
> // Variables and functions — lowercase with underscores
> int pid;
> void uart_putc(char c);
> ```
>
> The one near-universal rule: **macros are ALL CAPS.** This warns the
> reader "this is text substitution, not a function call — beware of
> double evaluation and other macro pitfalls." When you see `PGSIZE`,
> you know it's a macro. When you see `uart_putc`, you know it's a
> real function.

### `sizeof` — measuring types and objects

`sizeof` returns the size in bytes of a type or expression. It's
evaluated **at compile time** (not runtime), so it has zero cost.

```c
sizeof(char)      // always 1 (by definition)
sizeof(int)       // 4 on RV64
sizeof(long)      // 8 on RV64
sizeof(uint64)    // 8
sizeof(void *)    // 8 (all pointers are the same size)

int arr[10];
sizeof(arr)       // 40 (10 * 4 bytes)
sizeof(arr[0])    // 4
// number of elements:
int n = sizeof(arr) / sizeof(arr[0]);  // 10
```

**Important caveat:** when you pass an array to a function, it decays to
a pointer, so `sizeof` inside the function gives you the pointer size
(8), not the array size. This is a common gotcha:

```c
void foo(int arr[]) {
    sizeof(arr);  // 8 ← this is sizeof(int *), NOT sizeof the array!
}
```

This is why array-processing functions in C always take a separate
length parameter — there's no way to recover the array size from a
pointer.


---

## Part 4: Pointers

You've used pointers before, so we'll move quickly through basics and
focus on the patterns that show up in OS code.

### What a pointer is

A pointer is a variable that holds a **memory address**. That's it.
On RV64, addresses are 64 bits, so all pointers are 8 bytes.

```c
int x = 42;
int *p = &x;     // p holds the address of x

*p = 99;         // write 99 to the address p points to → x is now 99
int y = *p;      // read the value at the address → y is 99
```

The type before `*` tells the compiler what's **at** the address — how
many bytes to read/write and how to interpret them. `int *p` means "p
holds an address, and at that address there's a 4-byte int."

**Watch out: `*` means two different things.** In a **declaration**,
`*` means "this is a pointer type." In an **expression**, `*` means
"dereference — follow the address." Same character, different meaning:

```c
int *p = &x;     // declaration: * means "p is a pointer to int"
*p = 99;         // expression: * means "go to the address in p, write 99"
```

A common trap for newcomers:

```c
int *p;          // declares p as a pointer — uninitialized, holds garbage
*p = 5;          // dereferences garbage address → writes to random memory (BUG!)
```

This looks like it should "create a pointer and set its value to 5,"
but it doesn't. `int *p` declares the pointer (no memory allocated for
the target). `*p = 5` then dereferences whatever garbage address `p`
happens to contain. A pointer must point to valid memory before you
dereference it — either via `&` (address of a variable), an allocator
(`kalloc`), or a known hardware address (`0x10000000`).

### Pointer arithmetic

Adding an integer to a pointer advances it by that many **elements**,
not bytes:

```c
int arr[] = {10, 20, 30, 40};
int *p = arr;       // points to arr[0]

p + 1               // points to arr[1] — advanced by sizeof(int) = 4 bytes
p + 2               // points to arr[2] — advanced by 8 bytes
*(p + 2)            // same as arr[2] → 30
```

The compiler multiplies by `sizeof(*p)` automatically. This is why
pointer type matters for arithmetic:

```c
char *cp = (char *)arr;
cp + 1               // advanced by 1 byte (sizeof(char) = 1)

long *lp = (long *)arr;
lp + 1               // advanced by 8 bytes (sizeof(long) = 8)
```

In the assembly lecture, you saw this:
```asm
addi a0, a0, 8      # array++ (8 bytes per long)
```

That's the compiler turning `p++` on a `long *` into "add 8 bytes."

### Pointer subtraction

Subtracting two pointers gives the number of **elements** between them:

```c
int arr[] = {10, 20, 30, 40};
int *p = &arr[0];
int *q = &arr[3];

q - p           // 3 (three ints apart, not 12 bytes)
```

This is how `strlen` works conceptually — advance a pointer to the end,
subtract the start.

### `void *` — the generic pointer

`void *` is a pointer with no type information — it's just a raw
address. You can't dereference it (the compiler doesn't know the size)
or do arithmetic on it, but you can **cast** it to and from any other
pointer type:

```c
void *generic = &x;            // any pointer converts to void *
int *specific = (int *)generic; // cast back to use it

// Common in OS code:
void *page = kalloc();          // allocator returns raw memory
struct proc *p = (struct proc *)page;  // we give it a type
```

`void *` is C's way of saying "I'm passing around an address, and you
decide what's there." It's the foundation of generic interfaces —
`memcpy`, `memset`, and allocators all use `void *`.

### Casting pointers — the OS developer's power tool

In normal application code, casting between pointer types is rare and
suspect. In OS code, it's **routine** — you're constantly reinterpreting
raw memory:

```c
// Hardware register at a known physical address:
volatile uint8 *uart = (volatile uint8 *)0x10000000;

// Treating a page of memory as a struct:
void *page = kalloc();
struct proc *p = (struct proc *)page;

// Walking page tables: a PTE contains a physical address
uint64 pte = pagetable[index];
uint64 *next_table = (uint64 *)((pte >> 10) << 12);

// Casting between integer and pointer (for address math):
uint64 addr = (uint64)p;           // pointer → integer
struct proc *q = (struct proc *)addr;  // integer → pointer
```

The key insight: **pointers are just integers with type metadata
attached.** At the machine level, a pointer is a number in a register
— the CPU doesn't know or care whether it's "a pointer" or "a long."
The type metadata (`int *`, `char *`, `struct proc *`) exists only for
the compiler, telling it:
- How many bytes to read/write when you dereference (`*p`)
- How many bytes to advance when you do arithmetic (`p + 1`)
- What warnings to emit when you mix types

You could write an entire OS using only `long` and casts:

```c
// "Normal" C:
volatile char *uart = (volatile char *)0x10000000;
*uart = 'H';

// Equivalent — just long and casts:
long addr = 0x10000000;
*(volatile char *)addr = 'H';
```

Same machine code. The cast `(volatile char *)addr` converts the
integer to a pointer so the compiler knows to emit a 1-byte store.
Without the cast, the compiler would warn about assigning an integer to
a pointer — but the generated instruction would be identical.

This is why C is called "portable assembly" — the pointer abstraction
is so thin you can see through it to the machine underneath, and you
can drop through it entirely whenever you want.

### `const` — the read-only qualifier

`const` tells the compiler "this value should not be modified." If you
try to modify a `const` variable, the compiler errors at compile time
— it's a safety net, not a runtime check.

```c
const int x = 42;
x = 99;              // compiler error: assignment of read-only variable
```

**`const` with pointers — where it gets interesting.**

The placement of `const` relative to `*` controls *what* is read-only.
The trick: **read the declaration right-to-left from the variable
name**. Whatever `const` is adjacent to is what can't change. (This
right-to-left shortcut works for simple pointer declarations. For
complex cases involving arrays or function pointers, use the spiral
rule from Part 5 — it handles `const` too.)

```c
const char *s;        // right-to-left: "s is a pointer to const char"
                      // const is next to char → DATA is read-only
                      // s[0] = 'X' → error    s = "other" → OK

char *const s;        // right-to-left: "s is a const pointer to char"
                      // const is next to s → POINTER is read-only
                      // s[0] = 'X' → OK       s = "other" → error

const char *const s;  // both const — can't modify data OR pointer
```

The first form (`const char *s`) is by far the most common — it means
"I'm pointing to data I promise not to modify." The second form
(`char *const s`) is rare.

**`const` in function parameters** — communicating a contract:

```c
void uart_puts(const char *s);    // "I will read s but not modify it"
void memcpy(void *dst, const void *src, int n);  // dst is modified, src is not
```

The `const` on `src` tells both the compiler and the reader: this
function won't change your source data. If the implementation
accidentally writes to `src`, the compiler catches it. This is the
main use of `const` in kernel code — documenting and enforcing which
parameters are read-only.

**`const` does NOT mean "compile-time constant."** This trips up
programmers coming from C++, where `const int x = 42` *is* a
compile-time constant. In C, it's just a read-only variable — you
can't use it as an array size or `case` label:

```c
const int SIZE = 10;
int arr[SIZE];           // ERROR in C (OK in C++)
                         // use #define SIZE 10 instead
```

This is why kernel code uses `#define` for constants, not `const`
(Part 8 covers `#define` in detail).

**`const` and string literals.** A string literal like `"Hello"` is
an array of `char` stored in the `.rodata` section (read-only data),
with a null terminator: `{ 'H', 'e', 'l', 'l', 'o', '\0' }` — 6
bytes. The correct way to point to one:

```c
const char *s = "Hello";   // correct: s points to read-only data
char *s = "Hello";         // compiles but lies — data IS read-only
```

Both compile, but without `const` you lose the safety net. If you
accidentally write `s[0] = 'X'`, the `const` version catches it at
compile time. The non-`const` version compiles silently and is
undefined behavior at runtime.

**`const` in Java and Python:**

Java's `final` is C's `char *const` — the pointer (reference) can't
be reassigned, but the data is still mutable:

```c
// C equivalent of Java's final:
char *const s = buf;
s = other_buf;       // error — can't reassign the pointer
s[0] = 'X';          // OK — can modify the data

// Java:
// final StringBuilder sb = new StringBuilder("Hello");
// sb = new StringBuilder("World");  // error — can't reassign
// sb.append("!");                   // OK — can modify the object
```

C's `const char *s` is the opposite — what Java **cannot express**:

```c
const char *s = buf;
s = other_buf;       // OK — can reassign the pointer
s[0] = 'X';          // error — can't modify the data
```

Java has no syntax for "this reference points to an object you can't
modify." You'd have to make the class itself immutable (like `String`
is). That's a design choice in the class, not a constraint at the
reference site.

Python has no `const` at all — everything is mutable unless the type
itself is immutable (strings, tuples, frozensets).

### Pointers to pointers (and the level-N mental model)

C lets you stack pointer levels arbitrarily: `int *`, `int **`,
`int ***`, and so on. This looks intimidating, but there's a simple
mental model: **each `*` in the type is one `ld` (dereference) you need
to perform before you reach the final value.**

```c
int    x = 42;        // the value itself
int   *p = &x;        // one dereference to reach x:     *p    == 42
int  **pp = &p;       // two dereferences to reach x:    **pp  == 42
int ***ppp = &pp;     // three dereferences to reach x:  ***ppp == 42
```

In assembly terms, each `*` is another `ld`:

```asm
# *p:
ld  a0, 0(a0)         # one load — follow one address

# **pp:
ld  a0, 0(a0)         # first load — get p's value
ld  a0, 0(a0)         # second load — follow p to x

# ***ppp:
ld  a0, 0(a0)         # first load — get pp
ld  a0, 0(a0)         # second load — get p
ld  a0, 0(a0)         # third load — get x
```

There's no magic — each level is just "this variable holds the address
of the next variable." The CPU doesn't know how many levels deep it
is. It just does `ld` — load the value at this address — as many times
as you ask.

In practice, you'll never go beyond 2 levels (`**`) in our OS.

### Why use pointers at all?

If you're coming from Python or Java, you might wonder why C needs
pointers when those languages get by without them. The answer: those
languages use pointers too — they just hide them. Every Python object
and every Java reference is a pointer behind the scenes. C makes you
manage them explicitly.

Every pointer use falls into one of three cases:

| Use case | Why a pointer is needed | Example |
|----------|------------------------|---------|
| **Modify a variable via function** | C is pass-by-value — parameters are copies. A pointer lets you reach the original. | `alloc_proc(&p)` modifies the caller's `p` |
| **Arithmetic / type metadata** | The compiler uses the pointer type for sizing (`*p` reads N bytes) and scaling (`p+1` advances N bytes). | `(uint64 *)(addr)` tells the compiler to read 8 bytes |
| **Refer to large or fixed-location data** | Copying a 200-byte struct or a 4KB page table every time is wasteful. Hardware registers exist at fixed addresses — there's nothing to copy. | `struct proc *p = &ptable[i]` |

The third case is the most common in OS code. This is why 90% of
kernel code uses `struct proc *p` instead of `struct proc p` — you're
working with *the* process at its real location in memory, not a copy.
In Python/Java, this happens automatically (objects are always accessed
by reference). In C, you write the `*` yourself.

You might wonder: could you hide the pointer with a typedef to make C
look like Java?

```c
typedef struct proc *Process;    // hide the * inside the typedef

Process p = alloc_proc();        // C with typedef
p->pid = 1;

// vs Java:
// Process p = new Process();
// p.pid = 1;
```

This works, and the parallel is real — Java references *are* pointers
with friendlier syntax. But kernel codebases don't do this because you
lose visibility: when you see `Process p`, you can't tell if `p = q`
copies the whole struct or just the pointer. With `struct proc *p`, the
`*` makes it obvious — assignment copies the address, not the data.
Both xv6 and Linux keep the `*` visible everywhere. We'll follow the
same convention.

**Pointer-to-pointer (`**`) in practice:**

The first use case — "modify a variable via function" — is where
pointer-to-pointer shows up. C is pass-by-value, so to modify the
caller's variable you pass a pointer to it. Whatever type the variable
is, the parameter is one `*` deeper:

| Want to modify | Parameter type | Call site |
|----------------|---------------|-----------|
| `int x` | `int *p` | `foo(&x)` |
| `struct proc *p` | `struct proc **pp` | `foo(&p)` |

Example — a function that allocates a process and fills in the
caller's pointer:

```c
int alloc_proc(struct proc **pp) {
    struct proc *p = kalloc();
    if (!p) return -1;
    p->pid = next_pid++;
    *pp = p;           // modify the caller's pointer
    return 0;
}

// Caller:
struct proc *p;
alloc_proc(&p);        // after this, p points to the new proc
```

The same `**` pattern enables an elegant linked list trick — "pointer
to the link" — that avoids special-casing the head:

```c
void remove(struct proc **head, struct proc *target) {
    struct proc **pp = head;              // pp points to the head pointer
    while (*pp != target)
        pp = &(*pp)->next;               // advance: pp now points to prev->next
    *pp = target->next;                  // unlink: prev->next = target->next
}
```

`pp` always points to **the field that holds** the current node's
address — whether that's the `head` variable or some node's `.next`
member. The unlinking code is the same either way, no `if (target ==
*head)` special case needed.

We'll use both patterns when building process lists and free page
lists.

### NULL

`NULL` is a pointer guaranteed to be invalid — typically defined as
`((void *)0)` or simply `0`. Dereferencing `NULL` is undefined behavior.

In OS code, `NULL` means "nothing here" — a process has no parent, a
page table entry is empty, an allocation failed:

```c
struct proc *p = find_proc(pid);
if (p == NULL) {
    // process not found
}
```

Once we set up virtual memory (Phase 4), we'll **unmap** the zero page
so that NULL pointer dereferences cause a page fault trap — the hardware
catches the bug for us. Until then, a NULL dereference in bare metal
reads/writes address 0 — the behavior is unpredictable and depends on
what's mapped there. On QEMU's `virt` machine, address 0 is in the
ROM region, so it probably won't crash, but it won't do anything useful
either. No safety net until we have virtual memory.


---

## Part 5: Arrays and Pointer Arithmetic

### Arrays and the decay rule

An array in C is a contiguous block of elements in memory. Its type
is `int [4]` (or whatever size).

One key distinction to internalize: **an array name *is* the address,
not a variable that stores the address.** A pointer variable like
`int *p` occupies 8 bytes of storage on the stack and holds an address
value. An array name like `arr` has no separate storage — it's just a
compile-time label for the address where the data starts. There's no
8-byte "pointer variable" sitting in memory for `arr`.

```c
int *p = arr;    // p is a real variable (8 bytes on stack), holds arr's address
                 // &p gives you where the pointer variable lives — different from arr

int arr[4];      // arr IS the address 0x1000 (if that's where it was placed)
                 // there's no separate 8-byte slot holding 0x1000
```

This matters because it explains a quirk that most tutorials gloss
over: **almost every time you use an array name, the compiler silently
converts it to a pointer to the first element.** This silent conversion
is called **decay** — the array name produces its address as a value,
but there was never a pointer variable to begin with.

```c
int arr[4] = {10, 20, 30, 40};   // arr's actual type: int [4]

arr[2]          // arr decays to int *, then *(arr + 2) → 30
foo(arr)        // arr decays to int *, function receives a pointer
int *p = arr;   // arr decays to int *, assigned to p
```

In all three cases, `arr` (type `int [4]`) silently becomes `&arr[0]`
(type `int *`). The size information — the fact that it's 4 elements —
is **lost**. The function, the pointer, the expression only see "the
address of the first int."

**There are exactly two exceptions** where decay does NOT happen:

| Context | What happens instead | Result type |
|---------|---------------------|-------------|
| `sizeof(arr)` | Measures the full array | `16` (4 ints × 4 bytes) |
| `&arr` | Takes the address of the whole array | `int (*)[4]` |

Everything else — indexing, passing to functions, assigning to pointers,
arithmetic — triggers decay. This is why:

- `arr[i]` is literally defined as `*(arr + i)` — `arr` decays to a
  pointer, then arithmetic and dereference. Since addition is
  commutative, `2[arr]` is the same as `arr[2]` (both are `*(2 + arr)`).
  Please never write that — but it illustrates that `[]` is just pointer
  arithmetic, nothing more.
- `sizeof(arr)` gives 16 inside the declaring function, but `sizeof`
  on a function parameter gives 8 (pointer size) — because the
  parameter already decayed.
- `&arr` and `&arr[0]` produce the same numeric address but different
  types — `&arr[0]` is `int *` (address of one element), `&arr` is
  `int (*)[4]` (address of the whole array, no decay).

```c
int arr[4] = {10, 20, 30, 40};   // lives at address 0x1000

arr          // DECAY: int [4] → int *       — pointer to first element
&arr[0]      // explicit: address of first element — same as decayed arr
&arr         // NO DECAY: address of the whole array — type: int (*)[4]
```

All three give the same numeric value `0x1000`, but `+1` means
different things because the type differs:

```c
int *p = arr;          // decayed: type int *
p + 1                  // 0x1004 — advances by sizeof(int) = 4 bytes

int (*ap)[4] = &arr;   // no decay: type int (*)[4]
ap + 1                 // 0x1010 — advances by sizeof(int[4]) = 16 bytes
```

Same address, different type metadata, different arithmetic. This is
the decay rule in action.

### Decay and function parameters

When you pass an array to a function, it decays. The function only
receives a pointer — the size is gone:

```c
void foo(int arr[]) {
    // arr is really int *p here — the [] is decoration
    sizeof(arr);    // 8 (pointer size), NOT 40!
}

void foo(int arr[10]) {
    // the 10 is IGNORED by the compiler — still just int *
    sizeof(arr);    // still 8
}

void foo(int *arr) {
    // all three declarations above are identical to this
}
```

This is why array-processing functions in C always take a separate
length parameter — there's no way to recover the array size from a
decayed pointer:

```c
long max_array(long *arr, int n);    // pointer + length
```

### Reading complex declarations — the spiral rule

The parentheses in `int (*ap)[4]` matter. Without them you get a
completely different type:

```c
int (*ap)[4]      // ap is a pointer to an array of 4 ints  (one pointer)
int *ap[4]        // ap is an array of 4 pointers to int    (four pointers)
```

**The spiral rule** for reading complex C declarations: start at the
variable name, read rightward (brackets, parentheses), then leftward
(pointer stars, type), spiraling outward:

```
int *ap[4]        →  ap → [4] → * → int
                     "ap is an array of 4 pointers to int"

int (*ap)[4]      →  ap → * → [4] → int
                     "ap is a pointer to an array of 4 ints"
```

The same rule decodes function pointer syntax:

```
int *fp(int)      →  fp → (int) → * → int
                     "fp is a function taking int, returning int *"

int (*fp)(int)    →  fp → * → (int) → int
                     "fp is a pointer to a function taking int, returning int"

int (*fp[4])(int, int)  →  fp → [4] → * → (int,int) → int
                     "fp is an array of 4 pointers to functions
                      taking (int,int), returning int"
```

That last one is a dispatch table. In practice, you'd never write it
raw — you'd use `typedef`:

```c
typedef int (*binop_t)(int, int);
binop_t ops[4] = { add, sub, mul, div_fn };
```

The spiral rule is for **reading** complex declarations in existing
code. For **writing**, use `typedef` and keep it simple.

### Multi-dimensional arrays as flat memory

C stores multi-dimensional arrays in **row-major order** — rows are
contiguous in memory:

```c
int grid[2][3] = {
    {1, 2, 3},     // row 0
    {4, 5, 6}      // row 1
};
```

Memory layout:
```
Address:  base   +4   +8  +12  +16  +20
Value:      1    2    3    4    5    6
            ─── row 0 ──  ─── row 1 ──
```

`grid[r][c]` is at offset `(r * num_cols + c) * sizeof(int)` from the
base. This matters when you think about page tables — Sv39 uses a
3-level table, and each "level" is really an array of 512 entries. The
virtual address encodes the three indices.


---

## Part 6: Structs, Unions, and Memory Layout

### Structs — grouping related data

A struct is a way to bundle multiple variables into a single type:

```c
struct proc {
    int pid;            // 4 bytes
    int state;          // 4 bytes
    uint64 kstack;      // 8 bytes (kernel stack address)
    uint64 *pagetable;  // 8 bytes (pointer)
    char name[16];      // 16 bytes
};
```

Access members with `.` (direct) or `->` (through pointer):

```c
struct proc p;
p.pid = 1;

struct proc *pp = &p;
pp->pid = 1;           // equivalent to (*pp).pid = 1
```

`->` is syntactic sugar for dereferencing and then accessing — nothing
more. You'll see `->` everywhere in kernel code because most OS
structures are accessed through pointers.

### Memory layout and padding

The compiler lays out struct members **in declaration order**, but may
insert **padding** between members to satisfy alignment requirements.
Each type wants to be at an address that's a multiple of its size:

```c
struct example {
    char  a;     // offset 0, size 1
                 // 3 bytes of padding (b needs 4-byte alignment)
    int   b;     // offset 4, size 4
    char  c;     // offset 8, size 1
                 // 7 bytes of padding (struct total needs 8-byte alignment)
    long  d;     // offset 16, size 8
};
// sizeof(struct example) = 24
```

Why? Because misaligned access is slow (or faults) on most hardware.
The compiler inserts invisible padding bytes to keep each member
naturally aligned.

**The same members, reordered to eliminate padding:**

```c
struct example_packed {
    long  d;     // offset 0, size 8
    int   b;     // offset 8, size 4
    char  a;     // offset 12, size 1
    char  c;     // offset 13, size 1
                 // 2 bytes of padding (struct needs 8-byte alignment)
};
// sizeof(struct example_packed) = 16   ← saved 8 bytes!
```

Rule of thumb: order members from largest to smallest to minimize
padding. In practice, kernel structs are ordered for **readability**
and logical grouping, not packing — a few wasted bytes per struct
doesn't matter when you have 128MB of RAM.

### `offsetof` — finding member positions

`offsetof(type, member)` returns the byte offset of a member within a
struct. Defined in `<stddef.h>`:

```c
offsetof(struct example, a)  // 0
offsetof(struct example, b)  // 4
offsetof(struct example, d)  // 16
```

This is useful for going from a member pointer back to the containing
struct — a pattern used in Linux's `container_of` macro (less common in
xv6, but worth knowing).

### Unions — overlapping members

A `union` is like a struct, except **all members share the same
memory** — the union's size is the size of its largest member:

```c
union value {
    uint32 as_uint;
    int32  as_int;
    float  as_float;
};
// sizeof(union value) = 4  (all members are 4 bytes, they overlap)
```

Writing to one member and reading another reinterprets the same bits
through a different type — the only difference is how the compiler
interprets them:

```c
union value v;
v.as_uint = 0xC1200000;       // write 4 bytes as unsigned int

uint32 a = v.as_uint;         // 3240099840 (large positive unsigned)
int32  b = v.as_int;          // -1054867456 (negative — high bit is 1)
float  c = v.as_float;        // -10.0 (IEEE 754 interprets those bits as -10.0)
```

All three read the same 4 bytes from the same memory location. This is
the same concept as pointer casting (`*(float *)&val`), just with
cleaner syntax.

In OS code, unions appear when a field can hold different things
depending on context:

```c
// A page table entry might be interpreted as:
union pte {
    uint64 raw;           // the full 64-bit value
    struct {
        uint64 v : 1;     // valid bit
        uint64 r : 1;     // readable
        uint64 w : 1;     // writable
        uint64 x : 1;     // executable
        // ... more fields
    } flags;
};
```

(The `: 1` syntax defines **bit fields** — members that occupy specific
bits rather than whole bytes. The compiler handles the masking and
shifting. We'll use manual bit manipulation instead in this project,
as it maps more directly to the hardware spec, but you should recognize
the syntax.)

### Designated initializers

C99 lets you initialize specific struct members by name:

```c
struct proc p = {
    .pid = 1,
    .state = RUNNABLE,
    .name = "init",
};
```

Unmentioned members are zero-initialized. This is clearer and more
maintainable than positional initialization (`{1, RUNNABLE, 0, 0,
"init"}`), and xv6 uses it throughout.

**Important:** the zero-initialization only applies when you use an
initializer (`= { ... }`). Without one, the behavior depends on where
the variable lives:

```c
// With initializer — unmentioned members are zero:
struct proc p = { .pid = 1 };     // state = 0, kstack = 0, name = all zeros
struct proc q = { 0 };            // all members zero (common idiom)

// No initializer, local variable — members are GARBAGE:
struct proc p;                     // uninitialized! all members are random

// No initializer, global/static — members are zero:
static struct proc ptable[64];    // C guarantees these are zeroed
```

> **Who actually zeros global/static variables?**
>
> The C standard *guarantees* that global and static variables start as
> zero. But someone has to do the zeroing — it's not magic. The answer
> depends on where the variable ends up in the ELF binary:
>
> | ELF section | What goes there | Zeroed by |
> |------------|----------------|-----------|
> | `.data` | Globals with non-zero initializers (`int x = 42`) | Not zeroed — has actual values, stored in the ELF file |
> | `.bss` | Globals without initializers or initialized to zero | The OS loader (on Linux) or `clear_bss()` (bare metal) |
> | `.rodata` | String literals, `const` globals | Not zeroed — has actual values |
> | Stack | Local variables | **Nobody** — contains whatever was there before |
>
> `.bss` is the key section. It doesn't store any data in the ELF
> file — it just records "allocate N bytes and zero them." On Linux,
> the kernel's ELF loader zeros `.bss` pages when loading your
> program. On bare metal (our case), we have to do it ourselves —
> that's why we'll write `clear_bss()` in Phase 1, looping from
> `_bss_start` to `_bss_end` and writing zeros (as shown in Part 11).
>
> QEMU happens to zero all RAM at startup, so we get away without
> `clear_bss()` for now. But on real hardware, RAM contains random
> garbage at power-on — skipping the zeroing would mean "uninitialized"
> globals start with whatever noise the DRAM chips powered up with.

For arrays:

```c
char *syscall_names[] = {
    [SYS_fork]  = "fork",
    [SYS_exit]  = "exit",
    [SYS_read]  = "read",
};
```

This is how xv6 builds dispatch tables indexed by syscall number.


---

## Part 7: Bitwise Operations

This is the section that will come up the most frequently in our OS.
Page table entries, CSR flags, interrupt masks, device status registers
— all of these are bitfields packed into integers, manipulated one bit
at a time.

### The six operators

| Operator | Name | C syntax | What it does |
|----------|------|----------|-------------|
| AND | Bitwise AND | `a & b` | Each result bit is 1 only if **both** input bits are 1 |
| OR | Bitwise OR | `a \| b` | Each result bit is 1 if **either** input bit is 1 |
| XOR | Bitwise XOR | `a ^ b` | Each result bit is 1 if the input bits **differ** |
| NOT | Bitwise NOT | `~a` | Flip every bit |
| Left shift | | `a << n` | Shift bits left by n positions, fill with 0 |
| Right shift | | `a >> n` | Shift bits right by n positions |

These operate on **each bit independently** (except shifts, which move
all bits together). If you know them from Python, they work identically
in C.

### The four fundamental operations

Almost everything you do with bits comes down to four patterns:

**Set a bit** (force it to 1):

```c
flags |= (1 << bit);
// Example: set bit 3
// flags = xxxx0xxx  →  flags = xxxx1xxx
```

OR with a 1-bit sets it. OR with a 0-bit leaves it unchanged.

**Clear a bit** (force it to 0):

```c
flags &= ~(1 << bit);
// Example: clear bit 3
// ~(1 << 3) = ~00001000 = 11110111
// flags = xxxx1xxx  →  flags = xxxx0xxx
```

AND with a 0-bit clears it. AND with a 1-bit leaves it unchanged.
`~(1 << bit)` creates a mask with all 1s except the target bit.

**Toggle a bit** (flip it):

```c
flags ^= (1 << bit);
// XOR with 1 flips the bit, XOR with 0 leaves it unchanged
```

**Test a bit** (check if it's 1):

```c
if (flags & (1 << bit)) {
    // bit is set
}
```

AND isolates the bit. If the result is non-zero, the bit was set.

### Working with multi-bit fields

Many hardware values pack multiple fields into a single integer. A
RISC-V Sv39 page table entry (PTE), for example:

```
Bit 63       54 53             10 9   8   7   6   5   4   3   2   1   0
┌────────────┬─────────────────┬─────┬───┬───┬───┬───┬───┬───┬───┬───┐
│  Reserved  │    PPN[2:0]     │ RSW │ D │ A │ G │ U │ X │ W │ R │ V │
└────────────┴─────────────────┴─────┴───┴───┴───┴───┴───┴───┴───┴───┘
```

**Extracting a field** (read bits [start, start+width)):

```c
uint64 extract(uint64 val, int start, int width) {
    return (val >> start) & ((1UL << width) - 1);
}
```

Step by step:
1. `val >> start` — shift the field to bit position 0
2. `(1UL << width) - 1` — create a mask of `width` ones
   (e.g., width=3 → `0b111`)
3. AND them together — isolate the field

Example: extract the PPN (bits 10-53, 44 bits wide) from a PTE:
```c
uint64 ppn = (pte >> 10) & 0xFFFFFFFFFFF;  // 44 ones
// Or equivalently:
uint64 ppn = (pte >> 10) & ((1UL << 44) - 1);
```

**Inserting a field** (write bits without disturbing others):

```c
uint64 insert(uint64 val, int start, int width, uint64 field) {
    uint64 mask = ((1UL << width) - 1) << start;
    return (val & ~mask) | ((field << start) & mask);
}
```

Step by step:
1. Create a mask covering the target bits
2. `val & ~mask` — clear the target bits in the original value
3. `(field << start) & mask` — position the new value and clip it
4. OR them together

**The `UL` suffix** in `1UL` is important. Plain `1` is an `int`
(32-bit on RV64). Shifting a 32-bit value left by 32 or more is
**undefined behavior**. `1UL` makes it `unsigned long` (64-bit),
which is safe to shift up to 63 positions.

### Hex notation and bit patterns

Hex is the natural notation for bit patterns because each hex digit
maps to exactly 4 bits:

```
0x0 = 0000    0x4 = 0100    0x8 = 1000    0xC = 1100
0x1 = 0001    0x5 = 0101    0x9 = 1001    0xD = 1101
0x2 = 0010    0x6 = 0110    0xA = 1010    0xE = 1110
0x3 = 0011    0x7 = 0111    0xB = 1011    0xF = 1111
```

So `0xFF` = 8 ones, `0xFFF` = 12 ones, `0x3FF` = 10 ones (binary:
`0011 1111 1111`). You'll internalize these with practice.

### Power-of-two tricks

Two patterns that appear constantly:

```c
// Check if x is a power of 2:
if (x != 0 && (x & (x - 1)) == 0) { ... }
// Why: x has one bit set. x-1 flips that bit and sets all lower bits.
// AND gives 0 only if there was exactly one bit set.

// Align address down to a page boundary (4096 = 2^12):
addr & ~0xFFF
// Clears the bottom 12 bits — rounds down to nearest 4096 multiple.

// Align up:
(addr + 0xFFF) & ~0xFFF
// Round up by adding page_size-1, then round down.
```

Page alignment is everywhere in memory management. We'll define macros
for these in Phase 3.


---

## Part 8: The C Preprocessor

The preprocessor runs **before** the compiler sees your code. It
operates on text — it knows nothing about C syntax, types, or scope.
It's a separate pass that transforms the source text and then hands the
result to the real compiler.

### `#include` — textual inclusion

We covered this in Part 2. Two forms:

```c
#include <stddef.h>       // search system include paths only
#include "uart.h"         // search current directory first, then fall back to system paths
```

The `""` form is a superset of `<>` — if the file isn't found locally,
it falls back to system paths. You could write `#include "stddef.h"`
and it would work (fail locally, find it in system paths). The
convention is just for clarity: `<>` signals "system/library header,"
`""` signals "our own project header."

You can also use relative paths: `#include "../drivers/uart.h"`. But
this is fragile — if you move a file, all relative paths break. The
cleaner approach is the `-I` compiler flag, which adds a directory to
the include search path:

```bash
gcc -I kernel/ -c proc/scheduler.c -o scheduler.o
```

Then in the source file:

```c
#include "drivers/uart.h"    // found via -I kernel/ → kernel/drivers/uart.h
```

In the Makefile, this becomes `CFLAGS += -I kernel/`. xv6 keeps it
simple — all kernel files live in one flat directory, so every include
is just `#include "uart.h"` with no paths. We'll follow the same
convention.

### `#define` — macros

**Object-like macros** (constants):

```c
#define PGSIZE    4096
#define UART_BASE 0x10000000UL
#define NULL      ((void *)0)
```

The preprocessor replaces every occurrence of `PGSIZE` with `4096` in
the source text. No type checking, no scope — pure text substitution.

**Why not `const int`?** In C (unlike C++), a `const` variable isn't a
true compile-time constant — you can't use it as an array size or in a
`case` label. `#define` gives you a literal value that works everywhere.
For integer constants, macros are the standard choice in C kernel code.

**Function-like macros:**

```c
#define MAX(a, b)  ((a) > (b) ? (a) : (b))
#define MIN(a, b)  ((a) < (b) ? (a) : (b))
#define ROUNDUP(n, sz)  (((n) + (sz) - 1) & ~((sz) - 1))
```

### Macro pitfalls

Macros are text substitution, which creates traps for the unwary:

**Pitfall 1: Operator precedence.**

```c
#define DOUBLE(x)  x * 2

DOUBLE(3 + 4)   // expands to: 3 + 4 * 2 = 11, not 14!
```

Fix: **always parenthesize** the macro body and every parameter:

```c
#define DOUBLE(x)  ((x) * 2)

DOUBLE(3 + 4)   // expands to: ((3 + 4) * 2) = 14 ✓
```

**Pitfall 2: Double evaluation.**

```c
#define MAX(a, b)  ((a) > (b) ? (a) : (b))

MAX(i++, j)     // expands to: ((i++) > (j) ? (i++) : (j))
                // i gets incremented TWICE if i > j!
```

There's no clean fix for macros — this is a fundamental limitation.
For anything more than trivial expressions, use a `static inline`
function instead (covered in Part 11). For simple constants and
address calculations, macros are fine.

**Pitfall 3: Multi-statement macros.**

```c
#define SWAP(a, b)  int tmp = a; a = b; b = tmp;

if (x > y)
    SWAP(x, y);    // only the first statement is inside the if!
```

Fix: wrap in `do { ... } while(0)`:

```c
#define SWAP(a, b)  do { int tmp = (a); (a) = (b); (b) = tmp; } while(0)

if (x > y)
    SWAP(x, y);    // expands to: do { ... } while(0); — single statement ✓
```

The `do { } while(0)` idiom creates a block that syntactically acts
like a single statement, including requiring a trailing semicolon at
the call site. You'll see this pattern throughout Linux and xv6.

### Include guards

Without protection, including the same header twice causes "redefinition"
errors:

```c
// If main.c includes both proc.h and vm.h, and vm.h also includes proc.h:
// struct proc gets defined twice → compiler error

// Fix — include guard in proc.h:
#ifndef PROC_H
#define PROC_H

struct proc {
    int pid;
    // ...
};

#endif  // PROC_H
```

The first time `proc.h` is included, `PROC_H` is not defined, so the
contents are processed and `PROC_H` gets defined. The second time,
`PROC_H` is already defined, so `#ifndef` is false and the contents
are skipped. Every header file should have an include guard.

Most modern compilers (GCC, Clang, MSVC) support a shortcut:

```c
#pragma once

struct proc {
    int pid;
    // ...
};
```

`#pragma once` does the same thing as include guards — the compiler
tracks which files have been included and skips duplicates. It's less
error-prone (no risk of guard name typos like `PROC_H` vs `PORC_H`)
and is what most modern C++ projects use. However, it's not part of the
C standard — it's a compiler extension. We'll use the traditional
`#ifndef` guards to stay consistent with xv6's style.

### Conditional compilation

```c
#ifdef DEBUG
    kprintf("debug: x = %d\n", x);
#endif

#if NCPU > 1
    acquire(&lock);       // only compiled for multi-core builds
#endif
```

This is how you include or exclude code at compile time — the excluded
code doesn't exist in the binary at all. xv6 uses this sparingly;
Linux uses it extensively for platform-specific code.

### Stringify and token pasting

Two advanced features you'll occasionally see:

```c
#define STR(x)     #x            // turns argument into a string literal
#define CONCAT(a, b)  a##b       // pastes two tokens together

STR(hello)      // "hello"
CONCAT(uart, _putc)  // uart_putc
```

`#x` is "stringify" — wraps the argument in quotes. `a##b` is "token
pasting" — glues two identifiers together. Used in macros that generate
function names or build tables. Not something you'll write often, but
you should recognize the syntax.


---

## Part 9: Undefined Behavior

This is the section that matters most if you're coming from Python or
Java, because those languages don't have this concept. **Undefined
behavior (UB)** means the C standard imposes no requirements on what
happens — the compiler can do anything, including generating code that
"works" on your machine but breaks on a different optimization level or
compiler version.

In Java, every edge case has a defined result — integer overflow wraps
silently, null dereference throws `NullPointerException`, array
out-of-bounds throws `IndexOutOfBoundsException`. The JVM guarantees
these behaviors on every platform. In Python, similar guarantees hold —
integers have arbitrary precision (no overflow at all), and bad accesses
raise exceptions. C makes no such promises.

### Why UB exists

C is designed to run on wildly different hardware — from 8-bit
microcontrollers to 64-bit supercomputers. Rather than mandate a
specific behavior for edge cases (which might be expensive on some
hardware), the standard says "if you do this, all bets are off." This
gives the compiler freedom to generate faster code by assuming UB never
happens.

### The UB the compiler exploits

The dangerous part: modern compilers don't just generate "some random
result" for UB — they **assume UB never occurs** and use that
assumption to optimize. This means UB can make code disappear entirely:

```c
// Example: signed integer overflow
int x = INT_MAX;
x = x + 1;          // UB! Signed overflow is undefined.

// The compiler assumes this never happens. If it sees:
if (x + 1 > x)      // "always true if no overflow"
    do_something();  // compiler may remove the if entirely

// The compiler reasons: "signed overflow can't happen (it's UB),
// so x + 1 is always > x, so the if is always true, so I'll just
// emit do_something() unconditionally."
```

```c
// Example: null pointer dereference
void foo(int *p) {
    int x = *p;          // dereference p
    if (p == NULL)        // compiler: "p was already dereferenced,
        handle_null();    //  so p can't be NULL (that would be UB),
                          //  so I'll remove this check entirely"
}
```

These aren't hypothetical — GCC and Clang do this at `-O2`.

### The most common UB in systems code

| UB | What it looks like | What the compiler might do |
|----|--------------------|---------------------------|
| Signed overflow | `int x = INT_MAX + 1;` | Assume it doesn't happen; delete code that checks for it |
| Null dereference | `*p` when `p == NULL` | Assume p is always valid; remove NULL checks |
| Out-of-bounds access | `arr[10]` when `arr` has 5 elements | Read/write random memory; miscompile surrounding code |
| Uninitialized variable | `int x; return x;` | Return any value; optimize based on assumptions about x |
| Shift too far | `1 << 32` (int is 32-bit) | Any result; often 0 or the original value |
| Aliasing violation | Accessing `int *` through `float *` | Assume they don't alias; reorder or cache reads |

### How OS code handles UB

Kernel code routinely does things that *look* like UB — casting between
pointer types, accessing hardware at arbitrary addresses, treating raw
memory as structs. Some of these are technically UB per the standard,
but work in practice because:

1. **We use specific compiler flags** that tame the most dangerous
   optimizations. For example, `-fno-strict-aliasing` tells GCC "don't
   assume pointer types determine aliasing" — this makes type-punning
   casts safe.

2. **We use `volatile`** for hardware access, which prevents the
   compiler from making assumptions about memory.

3. **We use `unsigned` types for bit manipulation.** Unsigned overflow
   is **defined** (it wraps modulo 2^n) — only *signed* overflow is UB.
   This is why kernel code uses `uint64` everywhere instead of `long`.

4. **We use GCC-specific extensions** when the standard isn't enough
   (inline assembly, `__attribute__` annotations).

**Practical rules for our project:**
- Use unsigned types for bit manipulation and address arithmetic
- Never rely on signed overflow (use unsigned)
- Always initialize variables before use
- Use `volatile` for all hardware register access
- Use `1UL << n` (not `1 << n`) when shifting more than 30 positions


---

## Part 10: Function Pointers

In Java, you'd use an interface or lambda for callbacks. In Python,
functions are first-class values you pass around freely. C has the same
concept as Python, just with more verbose syntax and no closures.

### Syntax

```c
// Declare a function pointer variable:
int (*fp)(int, int);          // fp is a pointer to a function that
                               // takes two ints and returns an int

// Assign it:
int add(int a, int b) { return a + b; }
fp = add;                      // function name decays to pointer (like arrays)

// Call through it:
int result = fp(3, 4);        // calls add(3, 4) → 7
```

**Function name decay** works like array decay but simpler — a
function name always decays to a pointer to itself, with no exceptions
(no `sizeof` special case, no `&` type distinction). This means the
`&` and `*` are optional:

```c
fp = add;           // decay: add → &add (implicit)
fp = &add;          // explicit: same thing

fp(3, 4);           // call through pointer (implicit dereference)
(*fp)(3, 4);        // explicit dereference: same thing
```

All four lines generate identical machine code. Most code uses the
shorter forms (`fp = add` and `fp(3, 4)`).

The parentheses in `(*fp)` matter — see the spiral rule in Part 5 for
why `int (*fp)(int)` and `int *fp(int)` are completely different types.

### typedef makes it readable

The raw syntax is noisy. `typedef` cleans it up:

```c
typedef int (*binop_t)(int, int);

binop_t fp = add;
int result = fp(3, 4);
```

**How to read `typedef`:** the trick is that `typedef` uses exactly
the same syntax as a variable declaration — just prepend `typedef` and
the "variable name" becomes a type name:

```c
int (*fp)(int, int);              // fp is a VARIABLE — a function pointer
typedef int (*binop_t)(int, int); // binop_t is a TYPE — alias for the same thing
```

This works for any type, not just function pointers:

```c
int x;                    →  typedef int myint;        // myint = int
int *p;                   →  typedef int *intptr;      // intptr = int *
int arr[4];               →  typedef int vec4[4];      // vec4 = int [4]
int (*fp)(int, int);      →  typedef int (*binop_t)(int, int);
```

And since `typedef` uses the same declaration grammar, the spiral rule
(Part 5) decodes both — the only difference is whether the name refers
to a variable or a type.

### Dispatch tables — the OS pattern

The most important use of function pointers in our OS is **dispatch
tables** — arrays of function pointers indexed by some selector:

```c
// System call dispatch (Phase 6):
typedef uint64 (*syscall_fn_t)(void);

syscall_fn_t syscalls[] = {
    [SYS_fork]  = sys_fork,
    [SYS_exit]  = sys_exit,
    [SYS_read]  = sys_read,
    [SYS_write] = sys_write,
};

// In the syscall handler:
void syscall(void) {
    int num = get_syscall_number();
    uint64 result = syscalls[num]();   // dispatch by number
    // ...
}
```

Instead of a giant `switch` statement, you index into an array of
function pointers. The trap handler in Phase 2 uses a similar pattern
to dispatch different exception types.


---

## Part 11: Common Patterns in OS C Code

These are patterns you'll see repeatedly in xv6 and our kernel that
don't fit neatly into the previous categories.

### `volatile` — don't optimize this access

We saw `volatile` briefly in Lecture 0-1. Here's the full story.

The compiler aggressively optimizes memory access. If you write:

```c
uint8 *status = (uint8 *)0x10000005;
while (*status == 0) {
    // wait for device
}
```

The compiler may read `*status` once, see it's 0, and transform the
loop into an infinite loop that never re-reads — because nothing in
the visible C code changes `*status`. But the *hardware* changes it
asynchronously.

`volatile` tells the compiler: "this memory location can change at
any time for reasons you can't see. Never cache reads, never elide
writes, never reorder accesses."

```c
volatile uint8 *status = (volatile uint8 *)0x10000005;
while (*status == 0) {
    // compiler MUST re-read *status every iteration
}
```

**When to use `volatile` in our OS:**
- All memory-mapped I/O registers (UART, CLINT, PLIC, virtio)
- Shared data accessed by both interrupt handlers and normal code
  (until we implement proper locks)

**When NOT to use `volatile`:**
- Normal variables
- Data protected by locks (the lock acquire/release provides the
  necessary ordering)

### `static inline` — small functions in headers

We covered this in Lecture 0-2 (assembly recap, Part 6 — inline
assembly), but it's worth revisiting as a general C pattern.

```c
// In riscv.h:
static inline uint64 r_sstatus(void) {
    uint64 x;
    asm volatile("csrr %0, sstatus" : "=r"(x));
    return x;
}
```

- **`inline`** — substitute the function body at each call site (no
  actual function call overhead)
- **`static`** — each translation unit gets its own copy, preventing
  linker conflicts when the header is included from multiple `.c` files
- Combined — the compiler inlines it everywhere and no copy actually
  survives in the binary

Use for tiny functions (1-5 lines) that are called frequently.
Anything larger should be a normal function in a `.c` file.

### Linker-defined symbols

In `entry.S` and the linker script, you've already seen symbols like
`stack_top` that are defined by the linker, not by C code. You can
access these from C, but the syntax is surprising:

```c
// In the linker script:
//   _bss_start = .;
//   .bss : { *(.bss) }
//   _bss_end = .;

// In C:
extern char _bss_start[];
extern char _bss_end[];

// To zero out BSS:
void clear_bss(void) {
    for (char *p = _bss_start; p < _bss_end; p++)
        *p = 0;
}
```

**Why `extern char _bss_start[]` and not `extern char *_bss_start`?**

A linker symbol has an address but **no storage** — there's no pointer
variable in memory holding the address. The symbol's **value** is the
address. If you declare it as `extern char *_bss_start`, the compiler
thinks there's a pointer variable at that address and tries to read 8
bytes from there as a pointer value — which is wrong (those bytes are
whatever happens to be at the start of BSS, not a pointer).

Declaring it as `extern char _bss_start[]` (array) works because an
array name already *is* its address — the compiler uses `_bss_start`
as the address directly without trying to dereference it. The `[]` is
empty because the linker symbol has no known size, but that's fine —
we only need the address.

This is the standard idiom for accessing linker-defined symbols from C.

### Type punning through pointer casts

"Type punning" means accessing the same memory through different types.
This is the same concept we discussed in Part 4 — pointers are just
integers with type metadata, and you could write everything with `long`
and casts. Type punning is the common name for this practice:

```c
// The allocator returns raw memory — just an address, no type:
void *page = kalloc();

// We decide what's there by casting:
uint64 *pagetable = (uint64 *)page;   // "treat this as an array of uint64"
pagetable[0] = some_pte;

struct proc *p = (struct proc *)page;  // "treat this as a process struct"
p->pid = 1;
```

The memory is the same — the cast changes nothing at the machine level.
It's purely a compiler directive: "when I dereference this pointer, use
this type's size and layout." Same bytes, different interpretation —
exactly like the union example in Part 6.

This is safe as long as:
- The memory is properly aligned for the target type
- You're consistent about which type you use (don't read through
  `uint64 *` what you wrote through `struct proc *` concurrently)
- You compile with `-fno-strict-aliasing` (which we will)

### The `container_of` pattern

Given a pointer to a struct member, get back a pointer to the enclosing
struct:

```c
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

struct proc {
    int pid;
    struct list_node run_link;   // embedded in the struct
};

// Given a pointer to the run_link member:
struct list_node *node = dequeue(&run_queue);
struct proc *p = container_of(node, struct proc, run_link);
```

This works by subtracting the member's offset from its address to find
the struct's base address. It's the backbone of Linux's linked list
implementation. xv6 doesn't use it (it uses simpler linked lists), but
you'll encounter it in any Linux kernel code.


---

## Summary

Here's what we covered, mapped to where each concept appears in the
project:

| Concept | First appears in |
|---------|-----------------|
| No stdlib, freestanding headers | Phase 1 — building `kprintf` from scratch |
| Separate compilation, headers, linkage | Phase 1 — splitting kernel into `main.c`, `uart.c`, etc. |
| Exact-width types (`uint64`, etc.) | Phase 1 — every kernel source file |
| Pointers, casting, `void *` | Phase 1 — UART driver; Phase 3 — page allocator |
| Pointer-to-pointer | Phase 3 — free list; Phase 5 — process list |
| Structs and `->` | Phase 2 onward — `struct trapframe`, `struct proc` |
| Struct padding and alignment | Phase 4 — page tables must be page-aligned |
| Unions and bit fields | Phase 4 — page table entries |
| Bitwise operations | Phase 2 — CSR flags; Phase 4 — PTE manipulation |
| Bit field extract/insert | Phase 4 — Sv39 PTE decoding |
| Preprocessor macros | Phase 1 onward — `PGSIZE`, `NULL`, register offsets |
| Include guards | Phase 1 — every header file |
| Undefined behavior | Always — knowing what *not* to write |
| Function pointers, dispatch tables | Phase 2 — trap dispatch; Phase 6 — syscall table |
| `volatile` | Phase 1 — UART; Phase 2 — CLINT timer |
| `static inline` | Phase 2 — CSR accessor functions in `riscv.h` |
| Linker-defined symbols | Phase 1 — BSS clearing, kernel memory bounds |
| Type punning | Phase 3 — interpreting raw pages as typed data |


---

## Exercises

Four hands-on exercises in the `exercises/` directory let you practice
the most OS-relevant skills. See `exercises/GUIDE.md` for instructions.

| # | Function(s) | What it tests |
|---|-------------|--------------|
| 1 | `bitfield_extract`, `bitfield_insert` | Shifts and masks to extract/insert bit fields |
| 2 | `memset`, `memcpy` | Pointer arithmetic, byte-by-byte memory access |
| 3 | `sorted_insert` into a linked list | Structs, pointer-to-pointer, traversal |
| 4 | `format_hex` — format a `uint64` as hex | Shifts, masks, arrays, ASCII arithmetic — ties it all together |
