#!/usr/bin/env python3
"""Generate compile_commands.json for clangd / the LSP tooling.

bobchouOS builds with riscv-none-elf-gcc (see Makefile), but clangd runs the
host clang. This emits a compile database that points clang at the RISC-V
target and the cross-gcc freestanding headers, so navigation + diagnostics
work without false errors.

The output is machine-specific (hardcoded /opt/riscv paths) and gitignored;
re-run this after a clean checkout or a toolchain path change:

    python3 tools/gen-compile-commands.py

Notes:
- Arch string is rv64imac (NOT rv64imac_zicsr): clang 15 rejects the explicit
  'zicsr' extension token the Makefile passes to gcc. CSR instructions still
  parse fine under plain rv64imac. The real gcc build is unaffected.
- Only kernel/ and user/ sources are indexed; lectures/ standalone exercises
  are intentionally excluded.
"""

import json
import glob
import os

# Toolchain layout — HARDCODED for this machine. On a new machine (or after
# a toolchain upgrade) adjust these two to match your install:
#   RISCV_ROOT — where the riscv-none-elf cross toolchain lives
#   GCC_VER    — the installed cross-gcc version (sets the header search path)
# Check with: riscv-none-elf-gcc --version  and  ls /opt/riscv/lib/gcc/riscv-none-elf
RISCV_ROOT = "/opt/riscv"
GCC_VER = "15.2.0"
GCC = f"{RISCV_ROOT}/lib/gcc/riscv-none-elf/{GCC_VER}"
SYS = f"{RISCV_ROOT}/riscv-none-elf/include"

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

COMMON = [
    "clang",
    "--target=riscv64-unknown-elf",
    "-march=rv64imac", "-mabi=lp64",
    "-ffreestanding", "-nostdlib", "-mcmodel=medany",
    "-fno-strict-aliasing",
    "-Wall", "-g",
    f"-isystem{GCC}/include",
    f"-isystem{GCC}/include-fixed",
    f"-isystem{SYS}",
]

KERNEL_INC = ["-Iinclude", "-Ikernel/include", "-Ikernel"]
USER_INC = ["-Iinclude", "-Iuser"]


def main():
    os.chdir(ROOT)
    entries = []

    for f in sorted(glob.glob("kernel/**/*.c", recursive=True)):
        entries.append({
            "directory": ROOT,
            "arguments": COMMON + KERNEL_INC + ["-c", f],
            "file": f,
        })

    seen = {e["file"] for e in entries}
    for f in sorted(glob.glob("user/**/*.c", recursive=True)):
        if f in seen:
            continue
        seen.add(f)
        entries.append({
            "directory": ROOT,
            "arguments": COMMON + USER_INC + ["-c", f],
            "file": f,
        })

    with open("compile_commands.json", "w") as fh:
        json.dump(entries, fh, indent=2)

    print(f"wrote compile_commands.json ({len(entries)} entries)")


if __name__ == "__main__":
    main()
