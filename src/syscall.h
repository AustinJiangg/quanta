#ifndef QUANTA_SYSCALL_H
#define QUANTA_SYSCALL_H

#include "cpu.h"

/*
 * The system-call layer — the "kernel" side of the user/kernel boundary.
 *
 * A bare ISA can only move bits between registers and memory; it has no way to
 * reach the outside world. ECALL is the doorway: the program puts a syscall
 * number in a7 and arguments in a0..a5, then executes `ecall` to ask the
 * supervisor to act on its behalf. We follow the RISC-V Linux/newlib syscall
 * ABI, so programs built with the standard cross-toolchain Just Work.
 *
 * This is deliberately separate from cpu.c: the CPU models hardware, this
 * models the privileged software the hardware traps into.
 */

/* Service the ECALL at the current PC: read the syscall number from a7, act on
 * it, and place any return value in a0. May set cpu->halted and record a
 * HaltReason (the exit family stops the machine). */
void syscall_dispatch(CPU *cpu);

#endif /* QUANTA_SYSCALL_H */
