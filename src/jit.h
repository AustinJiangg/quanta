#ifndef QUANTA_JIT_H
#define QUANTA_JIT_H

#include <stdint.h>

/*
 * A basic-block JIT: from interpretation to dynamic binary translation (M25b).
 *
 * The decoded-instruction cache (M25a) removed the fetch/decode redundancy but
 * still dispatches every instruction through cpu_step's switch. This layer goes
 * the rest of the way: straight-line runs of guest instructions ("basic blocks",
 * ending at a branch/jump or anything that can redirect control) are translated
 * once into host x86-64 machine code operating directly on the CPU struct, and
 * re-executions run that native code instead of the interpreter loop. The
 * interpreter remains the bit-exact golden reference — a machine with the JIT
 * off must produce identical results, which `make check-jit` pins the same way
 * `make check-dcache` pins the decode cache.
 *
 * How bit-exactness survives multi-instruction execution. The interpreter
 * checks for interrupts and ticks the platform timer at *every* instruction
 * boundary; a translated block runs several instructions between checks. That
 * is only equivalent if no check inside the block would have fired:
 *
 *  - The dispatcher runs the ordinary step prologue (cpu_pre_step) first, so a
 *    pending interrupt is taken exactly where the interpreter would take it.
 *  - Time-driven interrupts (CLINT timer, the SBI STIP relay, Sstc) fire at a
 *    step count computable in advance: cpu_interrupt_horizon() bounds how many
 *    instructions may retire before one could become takeable, and a block
 *    longer than the horizon falls back to the interpreter.
 *  - Every other interrupt source changes only through device state, which a
 *    guest can only touch by MMIO: translated loads/stores go through a helper
 *    that detects a device-window physical address and *exits the block* after
 *    that instruction retires, so the very next boundary re-checks everything.
 *  - Instructions that can change the gates themselves (CSR access, mret/sret,
 *    ecall — all SYSTEM), atomics, floats, and FENCE.I end a block and run in
 *    the interpreter.
 *
 * The engine advances mtime by the block's consumed steps afterwards, so the
 * clocks (mtime, instret) read exactly as if each instruction had stepped.
 *
 * Blocks are keyed by *physical* PC like the decode cache (they survive satp
 * writes and sfence.vma), but tagged with the virtual PC too: a block embeds
 * VA-derived constants (AUIPC results, branch targets, link addresses), so the
 * same physical code mapped at a different virtual address must retranslate.
 * A block never crosses a page boundary (VA-contiguity is only guaranteed
 * within a page). The one event that changes what a physical address decodes
 * to is a store to it, which Zifencei requires a FENCE.I to make visible to
 * fetch — so cpu_step flushes the JIT on FENCE.I, exactly like the decode
 * cache, and tests/smc.S pins it.
 *
 * Precise state at every exit. Translated code keeps the guest registers in
 * the CPU struct (memory-resident; no register allocation yet), stores the
 * faulting instruction's address to cpu->pc before any helper that can trap,
 * and adds the exact retired count to cpu->instret on each exit path — so a
 * page fault out of a block is indistinguishable from one out of cpu_step.
 *
 * Scope: the JIT engages only on a uniprocessor (the M19 round-robin scheduler
 * interleaves SMP harts one instruction at a time — block execution would
 * change the interleaving) and only inside quanta_run (quanta_step, which the
 * GDB stub and --trace drive, stays pure interpreter). Host support is x86-64
 * with POSIX mmap; elsewhere jit_available() is 0 and the flag is refused.
 * This is the project's fourth OS-specific corner (after the console input,
 * the gdb stub's sockets, and the NAT backend's sockets).
 */

struct CPU;

typedef struct Jit Jit;

/* Can this build translate at all (x86-64 host with an executable-page
 * allocator)? When 0, jit_new returns NULL and --jit is refused up front. */
int jit_available(void);

/* Allocate a JIT (code arena + block table). NULL if unavailable or OOM. */
Jit *jit_new(void);

/* Release a JIT. Safe on NULL. */
void jit_free(Jit *j);

/* Invalidate every translated block: the FENCE.I flush (called from cpu_step
 * via cpu->jit) and the machine-restore flush. O(1) — a generation bump; the
 * code arena is recycled. Never called from inside a translated block (FENCE.I
 * is not translated), so resetting the arena is safe. Safe on NULL. */
void jit_flush(Jit *j);

/* Try to advance the hart from its current PC by translated code. Returns the
 * number of scheduler steps consumed (the caller adds them to its step count
 * and advances mtime by consumed - 1, the entry tick being its own), or 0 when
 * the caller should fall back to one cpu_step() — cold code, a fetch fault, a
 * block longer than the interrupt horizon or `max_steps`. A return of 1 may
 * also mean the prologue consumed the step (an interrupt was taken, or the
 * machine powered off); the caller's halt polling picks that up. */
uint64_t jit_run(Jit *j, struct CPU *cpu, uint64_t max_steps);

#endif /* QUANTA_JIT_H */
