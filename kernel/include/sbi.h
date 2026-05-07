/*
 * sbi.h — Mini-SBI interface (S-mode → M-mode ecall).
 *
 * Our own M-mode handler (m_vec.S) implements these. No external
 * firmware (OpenSBI) is involved — we run with -bios none.
 *
 * See Lecture 5-1, Part 4 ("How S-mode talks to the timer hardware").
 */

#ifndef SBI_H
#define SBI_H

/* SBI function IDs — dispatched by a7 in M-mode handler.
 * Shared between assembly (.S) and C files. */
#define SBI_SET_TIMER 0
#define SBI_SHUTDOWN 1

#ifndef __ASSEMBLER__

#include "types.h"

void sbi_set_timer(uint64 deadline);
void sbi_shutdown(void);

#endif /* !__ASSEMBLER__ */
#endif /* SBI_H */
