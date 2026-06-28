#ifndef QUANTA_RVC_H
#define QUANTA_RVC_H

#include <stdint.h>

/*
 * RV32C — the compressed-instruction extension (M11).
 *
 * RISC-V keeps code dense with a set of 16-bit instructions, each a shorthand
 * for a common 32-bit one (small immediates, the popular registers, the
 * stack-pointer forms). Rather than teach the executor and the disassembler a
 * second instruction format, we *expand*: a 16-bit instruction is widened to the
 * exact 32-bit instruction it stands for, and the existing decode/execute and
 * disassembly paths handle it unchanged. So this expander is the single source
 * of truth for what a compressed instruction means, shared by cpu.c (to run it)
 * and disasm.c (to print it — objdump shows the expanded mnemonic too).
 *
 * Whether a halfword is compressed is decided by its low two bits: a value other
 * than 0b11 is a 16-bit instruction; 0b11 introduces a 32-bit one. The caller
 * makes that test and only passes genuine compressed halfwords here.
 */

/* The expansion of an illegal/reserved compressed encoding: the all-zero word,
 * itself an illegal 32-bit instruction (and no valid expansion produces it, so
 * it is an unambiguous sentinel). The caller raises an illegal-instruction trap
 * — or, in the disassembler, prints the raw halfword. */
#define RVC_ILLEGAL 0u

/* Expand a 16-bit compressed instruction `c` to the equivalent 32-bit RV32I/M
 * instruction word, or RVC_ILLEGAL for a reserved/unsupported encoding (which
 * includes the RV32C floating-point forms, since F/D are not implemented). */
uint32_t rvc_expand(uint16_t c);

#endif /* QUANTA_RVC_H */
