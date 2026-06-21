#ifndef QUANTA_CPU_H
#define QUANTA_CPU_H

#include <stdint.h>
#include "memory.h"

/*
 * RV32I CPU state.
 *
 * The architectural state of an RV32I hart is small: 32 general-purpose
 * 32-bit registers plus a program counter. That is the entire "visible"
 * machine. Everything the processor does is a function that maps one such
 * state to the next.
 *
 * Register x0 is hardwired to zero in RISC-V: reads always return 0 and
 * writes are discarded. We enforce that in reg_write() rather than trusting
 * every instruction to respect it.
 */

/*
 * Why the machine stopped. cpu_step() and the syscall layer record one of
 * these in `halt_reason` whenever they set `halted`, so the embedder — the CLI
 * driver, or any program linking the core as a library — can tell a clean exit
 * from a fault. The core never calls exit() on its host; it stops and reports.
 */
typedef enum {
    HALT_NONE = 0,         /* still running */
    HALT_EXIT,             /* clean stop via the exit syscall */
    HALT_EBREAK,           /* ebreak with no debugger attached */
    HALT_ILLEGAL_INSN,     /* illegal or unimplemented instruction */
    HALT_UNIMP_SYSTEM,     /* unimplemented SYSTEM instruction (e.g. a CSR) */
    HALT_UNKNOWN_SYSCALL,  /* ecall with a syscall number we do not implement */
    HALT_MEM_FAULT         /* fetch/load/store outside mapped memory */
} HaltReason;

typedef struct {
    uint32_t regs[32];      /* x0..x31; x0 is always zero */
    uint32_t pc;            /* program counter */
    Memory  *mem;           /* attached memory (not owned by the CPU) */
    struct Cache *cache;    /* optional cache model; NULL if off (not owned) */
    int      halted;        /* set when execution should stop */
    HaltReason halt_reason; /* why it stopped (meaningful once halted) */
    uint32_t exit_code;     /* status the exit syscall passed in a0 */
} CPU;

/* Initialise a CPU: zero all registers, set PC to the given entry point,
 * and attach memory. */
void cpu_init(CPU *cpu, Memory *mem, uint32_t entry_pc);

/* Read register `i`. Returns 0 for x0. */
uint32_t reg_read(const CPU *cpu, uint32_t i);

/* Write `value` to register `i`. Writes to x0 are ignored. */
void reg_write(CPU *cpu, uint32_t i, uint32_t value);

/* Fetch, decode, and execute a single instruction at PC. Advances PC (by 4,
 * or to a branch/jump target). On a memory fault it records HALT_MEM_FAULT and
 * stops instead of advancing. */
void cpu_step(CPU *cpu);

/* A short human-readable name for a halt reason, for diagnostics. */
const char *halt_reason_str(HaltReason r);

#endif /* QUANTA_CPU_H */
