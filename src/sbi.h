#ifndef QUANTA_SBI_H
#define QUANTA_SBI_H

#include "cpu.h"

/*
 * SBI — the RISC-V Supervisor Binary Interface (M15).
 *
 * A supervisor-mode OS does not touch the machine's lowest-level controls
 * directly; it asks the firmware below it (M-mode: OpenSBI on real hardware)
 * for those services through the SBI, a calling convention layered on `ecall`.
 * S-mode executes `ecall`, which traps to M-mode; the firmware reads the SBI
 * extension id from a7, the function id from a6, and arguments from a0-a5, does
 * the work, and returns an (error, value) pair in (a0, a1).
 *
 * Quanta plays that firmware role. When an S-mode `ecall` traps with no guest
 * M-mode handler installed (mtvec still 0 — mtvec belongs to the firmware, which
 * here is us), the SEE routes it through `sbi_call` instead of the newlib syscall
 * layer that bare M/U-mode programs use. The supported surface is small but real:
 * the Base extension (probe/version), console putchar/getchar, a timer, hart
 * status, and system reset (which halts the machine).
 */

/* Service an SBI call against the hart's registers: dispatch on a7/a6, read
 * arguments from a0-a5, write the result to (a0, a1). A system-reset / shutdown
 * request stops the machine via HALT_EXIT rather than returning. */
void sbi_call(CPU *cpu);

#endif /* QUANTA_SBI_H */
