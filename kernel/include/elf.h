/*
 * elf.h — ELF64 binary format definitions for exec.
 *
 * Only the structures needed for loading executables (ELF header and
 * program headers). Section headers, symbol tables, etc. are not needed.
 *
 * See Lecture 6-3, Part 4.
 */

#ifndef ELF_H
#define ELF_H

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

struct elf_header {
    uint8 e_ident[16];
    uint16 e_type;
    uint16 e_machine;
    uint32 e_version;
    uint64 e_entry;
    uint64 e_phoff;
    uint64 e_shoff;
    uint32 e_flags;
    uint16 e_ehsize;
    uint16 e_phentsize;
    uint16 e_phnum;
    uint16 e_shentsize;
    uint16 e_shnum;
    uint16 e_shstrndx;
};

/* ELF64 program header (note: flags is before offset in 64-bit format) */
struct elf_phdr {
    uint32 p_type;
    uint32 p_flags;
    uint64 p_offset;
    uint64 p_vaddr;
    uint64 p_paddr;
    uint64 p_filesz;
    uint64 p_memsz;
    uint64 p_align;
};

#endif /* ELF_H */
