/*
 * elf_defs.h — ELF64 binary format definitions for exec.
 *
 * Named elf_defs.h (not elf.h) to avoid colliding with the system
 * <elf.h> header in IDE tooling.
 *
 * Only the structures needed for loading executables (ELF header and
 * program headers). Section headers, symbol tables, etc. are not needed.
 *
 * See Lecture 6-3, Part 4.
 */

#ifndef ELF_DEFS_H
#define ELF_DEFS_H

#include "types.h"

#define ELF_MAGIC 0x464C457F /* "\x7fELF" as a little-endian uint32 */

/* e_ident indices */
#define EI_CLASS 4
#define EI_DATA 5

/* e_ident values */
#define ELFCLASS64 2
#define ELFDATA2LSB 1

/* e_type */
#define ET_EXEC 2

/* e_machine */
#define EM_RISCV 0xF3

/* Program header types */
#define PT_LOAD 1

/* Program header flags */
#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

/* ELF64 file header — sits at offset 0 of the image. exec reads e_entry
 * (start PC), e_phoff/e_phnum (where the program headers are), and validates
 * e_ident[]/e_machine. The section-header fields (e_sh*) are unused. */
struct elf_header {
    uint8 e_ident[16]; /* magic + class/data/version; exec checks these */
    uint16 e_type;     /* ET_EXEC for a normal executable */
    uint16 e_machine;  /* EM_RISCV — reject binaries built for other CPUs */
    uint32 e_version;
    uint64 e_entry; /* virtual address of _start (-> trapframe->epc) */
    uint64 e_phoff; /* byte offset of the program-header table */
    uint64 e_shoff; /* section headers (unused) */
    uint32 e_flags;
    uint16 e_ehsize;
    uint16 e_phentsize; /* size of one program header */
    uint16 e_phnum;     /* number of program headers */
    uint16 e_shentsize;
    uint16 e_shnum;
    uint16 e_shstrndx;
};

/* ELF64 program header — one per segment. exec loads only PT_LOAD segments.
 * (Note: in ELF64 p_flags comes right after p_type, unlike ELF32 where flags
 * are last — a classic gotcha when porting 32-bit ELF code.) */
struct elf_phdr {
    uint32 p_type;   /* PT_LOAD = a segment to map into memory */
    uint32 p_flags;  /* PF_R/PF_W/PF_X -> PTE permission bits */
    uint64 p_offset; /* byte offset of segment data within the file */
    uint64 p_vaddr;  /* virtual address to load the segment at */
    uint64 p_paddr;  /* physical address (ignored for user programs) */
    uint64 p_filesz; /* bytes present in the file (copy these) */
    uint64 p_memsz;  /* bytes in memory; (memsz - filesz) is zeroed .bss */
    uint64 p_align;
};

#endif /* ELF_DEFS_H */
