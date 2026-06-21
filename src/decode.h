#ifndef QUANTA_DECODE_H
#define QUANTA_DECODE_H

#include <stdint.h>

/* ------------------------------------------------------------------------
 * Shared RV32I instruction decoding.
 *
 * Field extraction, immediate decoding, the opcode map, and ABI register
 * names live here so the executor (cpu.c) and the disassembler (disasm.c)
 * decode through one source of truth. The immediate bits are scrambled across
 * the instruction word; re-deriving them by hand is the easiest way to
 * introduce bugs, so these helpers are the only place it happens.
 *
 * Everything is `static inline`: each translation unit gets its own copy, the
 * compiler inlines it, and helpers a given unit doesn't use don't warn under
 * -Wall -Wextra (unlike a plain unused `static` function).
 * ------------------------------------------------------------------------ */

/* ------------------------------------------------------------------------
 * Instruction field extraction.
 *
 * An RV32I instruction is a single 32-bit word. Different "formats" (R, I,
 * S, B, U, J) slice that word into fields in fixed bit positions.
 *
 * Bit layout reference (RV32I):
 *   opcode = inst[6:0]
 *   rd     = inst[11:7]
 *   funct3 = inst[14:12]
 *   rs1    = inst[19:15]
 *   rs2    = inst[24:20]
 *   funct7 = inst[31:25]
 * ------------------------------------------------------------------------ */

static inline uint32_t opcode(uint32_t inst) { return inst & 0x7f; }
static inline uint32_t rd    (uint32_t inst) { return (inst >> 7)  & 0x1f; }
static inline uint32_t funct3(uint32_t inst) { return (inst >> 12) & 0x07; }
static inline uint32_t rs1   (uint32_t inst) { return (inst >> 15) & 0x1f; }
static inline uint32_t rs2   (uint32_t inst) { return (inst >> 20) & 0x1f; }
static inline uint32_t funct7(uint32_t inst) { return (inst >> 25) & 0x7f; }

/*
 * Immediates are the fiddly part of RV32I. The bits of an immediate are
 * scattered across the instruction word (a deliberate hardware trade-off:
 * it keeps the sign bit and register fields in fixed places across formats).
 * Each format reassembles them differently, and most are sign-extended.
 *
 * We sign-extend by casting the assembled value to int32_t after placing the
 * sign bit, relying on arithmetic right shift of a signed value.
 */

/* I-type: inst[31:20], sign-extended. */
static inline int32_t imm_i(uint32_t inst) {
    return (int32_t)inst >> 20;
}

/* S-type: inst[31:25] | inst[11:7], sign-extended. */
static inline int32_t imm_s(uint32_t inst) {
    uint32_t imm = ((inst >> 25) & 0x7f) << 5
                 | ((inst >> 7)  & 0x1f);
    /* sign-extend from bit 11 */
    if (imm & 0x800) imm |= 0xfffff000;
    return (int32_t)imm;
}

/* B-type: branch offset, bits scrambled, multiple of 2, sign-extended. */
static inline int32_t imm_b(uint32_t inst) {
    uint32_t imm = ((inst >> 31) & 0x1)  << 12
                 | ((inst >> 7)  & 0x1)  << 11
                 | ((inst >> 25) & 0x3f) << 5
                 | ((inst >> 8)  & 0xf)  << 1;
    if (imm & 0x1000) imm |= 0xffffe000;
    return (int32_t)imm;
}

/* U-type: inst[31:12] placed in the high 20 bits. */
static inline int32_t imm_u(uint32_t inst) {
    return (int32_t)(inst & 0xfffff000);
}

/* J-type: jump offset, bits scrambled, multiple of 2, sign-extended. */
static inline int32_t imm_j(uint32_t inst) {
    uint32_t imm = ((inst >> 31) & 0x1)   << 20
                 | ((inst >> 12) & 0xff)  << 12
                 | ((inst >> 20) & 0x1)   << 11
                 | ((inst >> 21) & 0x3ff) << 1;
    if (imm & 0x100000) imm |= 0xffe00000;
    return (int32_t)imm;
}

/* ------------------------------------------------------------------------
 * Major opcode values from the RV32I encoding. Both the executor's dispatch
 * and the disassembler switch on these.
 * ------------------------------------------------------------------------ */
enum {
    OP_LUI    = 0x37,
    OP_AUIPC  = 0x17,
    OP_JAL    = 0x6f,
    OP_JALR   = 0x67,
    OP_BRANCH = 0x63, /* BEQ/BNE/BLT/BGE/BLTU/BGEU */
    OP_LOAD   = 0x03, /* LB/LH/LW/LBU/LHU         */
    OP_STORE  = 0x23, /* SB/SH/SW                 */
    OP_FENCE  = 0x0f, /* FENCE / FENCE.I          */
    OP_IMM    = 0x13, /* ADDI/SLTI/.../SRAI       */
    OP_REG    = 0x33, /* ADD/SUB/.../AND          */
    OP_SYSTEM = 0x73  /* ECALL/EBREAK/CSR         */
};

/* ------------------------------------------------------------------------
 * Zicsr CSR addresses referenced by name in the core and the disassembler.
 * The unprivileged counters are read-only (their address's top two bits are
 * 0b11) and read back a live value; mscratch is a plain read/write CSR used
 * by the conformance test. The full privileged CSR set arrives with M9.
 * ------------------------------------------------------------------------ */
enum {
    CSR_MSCRATCH = 0x340,
    CSR_CYCLE    = 0xc00, CSR_TIME  = 0xc01, CSR_INSTRET  = 0xc02,
    CSR_CYCLEH   = 0xc80, CSR_TIMEH = 0xc81, CSR_INSTRETH = 0xc82
};

/* ABI register names (x0..x31), handy for register dumps and disassembly. */
static inline const char *reg_abi_name(uint32_t i) {
    static const char *names[32] = {
        "zero", "ra", "sp", "gp", "tp",  "t0", "t1", "t2",
        "s0",   "s1", "a0", "a1", "a2",  "a3", "a4", "a5",
        "a6",   "a7", "s2", "s3", "s4",  "s5", "s6", "s7",
        "s8",   "s9", "s10","s11","t3",  "t4", "t5", "t6"
    };
    return names[i & 0x1f];
}

#endif /* QUANTA_DECODE_H */
