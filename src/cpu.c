#include "cpu.h"
#include "decode.h"
#include "rvc.h"
#include "syscall.h"
#include "sbi.h"
#include "cache.h"
#include "mmu.h"
#include "device.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------
 * The instruction core.
 *
 * Field extraction, immediate decoding, the opcode map, and ABI register
 * names live in decode.h, shared with the disassembler so both decode an
 * instruction through one source of truth. Each instruction group below has
 * its own exec_* function; cpu_step() fetches a word, dispatches on the
 * opcode, and advances PC. Unimplemented encodings trap and halt the machine.
 * ------------------------------------------------------------------------ */

/* sstatus/sie/sip are masked windows onto mstatus/mie/mip; these masks pick the
 * bits S-mode may see and change. The mstatus bit fields and the exception
 * cause codes they build on are shared with the MMU, so they live in cpu.h. */
enum {
    SSTATUS_MASK = MSTATUS_SIE | MSTATUS_SPIE | MSTATUS_SPP |
                   MSTATUS_SUM | MSTATUS_MXR,
    S_INT_MASK = (1u << 1) | (1u << 5) | (1u << 9)
};

/* Interrupt numbers: the bit positions in mie/mip, and the low bits of mcause
 * with its high bit set. Machine outranks supervisor, and within a level the
 * delivery priority is external > software > timer. */
enum {
    IRQ_SSOFT  = 1,  IRQ_MSOFT  = 3,
    IRQ_STIMER = 5,  IRQ_MTIMER = 7,
    IRQ_SEXT   = 9,  IRQ_MEXT   = 11
};

/* Defined below with the trap machinery, declared here so the memory-access
 * instructions (which appear earlier) can raise page faults through it. */
static void raise_trap(CPU *cpu, uint32_t cause, uint64_t tval);

/* Sign-extend `v` from bit XLEN-1: in RV32 the low 32 bits are sign-extended
 * into the upper half (so every register keeps the sext invariant), in RV64 it
 * is the identity. The single helper every XLEN-dependent result funnels
 * through — reg_write applies it, so the executor mostly stays width-agnostic. */
static inline uint64_t sext_xlen(const CPU *cpu, uint64_t v) {
    return cpu->xlen == 32 ? (uint64_t)(int64_t)(int32_t)v : v;
}

/* The interrupt bit of mcause/scause: the top bit of XLEN (bit 31 in RV32, bit
 * 63 in RV64). Set in the cause for an asynchronous trap, clear for an
 * exception; a handler tests it to tell the two apart. */
static inline uint64_t cause_interrupt(const CPU *cpu) {
    return (uint64_t)1 << (cpu->xlen - 1);
}

void cpu_init(CPU *cpu, Memory *mem, uint64_t entry_pc, int xlen) {
    cpu->xlen      = xlen; /* 32 (RV32) or 64 (RV64), chosen by the loader */
    for (int i = 0; i < 32; i++) cpu->regs[i] = 0;
    cpu->pc        = entry_pc;
    cpu->mem       = mem;
    cpu->cache     = NULL;
    cpu->halted      = 0;
    cpu->halt_reason = HALT_NONE;
    cpu->exit_code   = 0;
    cpu->instret     = 0;
    cpu->priv        = PRIV_M; /* the hart resets into Machine mode */
    cpu->trapped     = 0;
    cpu->sbi_timer_armed = 0;
    cpu->reserve_valid = 0;
    cpu->reserve_addr  = 0;
    mmu_flush(cpu); /* invalidate the TLB */
    memset(cpu->csr, 0, sizeof cpu->csr);
    /* misa advertises the base and extensions: MXL in the top two XLEN bits (1
     * for RV32, 2 for RV64) plus C, I, M, S, U. Informational here; reads see it,
     * writes are accepted as WARL storage. */
    uint64_t mxl = (xlen == 64) ? 2u : 1u;
    cpu->csr[CSR_MISA] = (mxl << (xlen - 2)) |
                         (1u << 2) | (1u << 8) | (1u << 12) | (1u << 18) | (1u << 20);
}

uint64_t reg_read(const CPU *cpu, uint32_t i) {
    return (i == 0) ? 0u : cpu->regs[i];
}

void reg_write(CPU *cpu, uint32_t i, uint64_t value) {
    if (i != 0) cpu->regs[i] = sext_xlen(cpu, value); /* x0 stays zero */
}

/* RV-A: a store to the reserved word breaks any outstanding LR reservation, so
 * a later SC to it fails. Modelled at word granularity, which is enough for a
 * single hart — there are no other agents to race the reservation. */
static void break_reservation(CPU *cpu, uint64_t addr) {
    if (cpu->reserve_valid && (addr & ~(uint64_t)0x3) == cpu->reserve_addr)
        cpu->reserve_valid = 0;
}

/* Execute OP-IMM: register/immediate arithmetic (ADDI, SLTI, shifts, ...).
 *
 * Width-agnostic: operands are read at full width and the sext invariant means
 * add/compare/logic give the right XLEN result once reg_write re-sign-extends.
 * Only the shifts depend on XLEN — the shift amount is 5 bits in RV32, 6 in
 * RV64, and the right shifts must work on the architectural width, not the
 * sign-extended 64-bit container. SRAI vs SRLI is bit 30 (it survives the wider
 * RV64 shamt, where the funct7 test would not). */
static void exec_op_imm(CPU *cpu, uint32_t inst) {
    uint64_t a    = reg_read(cpu, rs1(inst));
    int64_t  imm  = imm_i(inst);                       /* sign-extended to XLEN */
    uint32_t shamt = (uint32_t)imm & (cpu->xlen == 64 ? 0x3fu : 0x1fu);
    uint32_t arith = (inst >> 30) & 1;                 /* SRAI when set, else SRLI */
    uint64_t result = 0;

    switch (funct3(inst)) {
        case 0x0: result = a + (uint64_t)imm; break;                 /* ADDI  */
        case 0x2: result = ((int64_t)a < imm) ? 1 : 0; break;        /* SLTI  */
        case 0x3: result = (a < (uint64_t)imm) ? 1 : 0; break;       /* SLTIU */
        case 0x4: result = a ^ (uint64_t)imm; break;                 /* XORI  */
        case 0x6: result = a | (uint64_t)imm; break;                 /* ORI   */
        case 0x7: result = a & (uint64_t)imm; break;                 /* ANDI  */
        case 0x1: result = a << shamt; break;                        /* SLLI  */
        case 0x5:
            if (arith)                                               /* SRAI  */
                result = cpu->xlen == 64 ? (uint64_t)((int64_t)a >> shamt)
                                         : (uint64_t)((int32_t)a >> shamt);
            else                                                     /* SRLI  */
                result = cpu->xlen == 64 ? (a >> shamt)
                                         : ((uint32_t)a >> shamt);
            break;
    }
    reg_write(cpu, rd(inst), result);
}

/* High 64 bits of the 128-bit unsigned product a*b, computed from 32-bit
 * partials so no 128-bit integer type is needed (this stays portable C11). The
 * signed high-half multiplies derive from this by a sign correction. */
static uint64_t mulhu64(uint64_t a, uint64_t b) {
    uint64_t alo = (uint32_t)a, ahi = a >> 32;
    uint64_t blo = (uint32_t)b, bhi = b >> 32;
    uint64_t lo_lo = alo * blo;
    uint64_t lo_hi = alo * bhi;
    uint64_t hi_lo = ahi * blo;
    uint64_t hi_hi = ahi * bhi;
    uint64_t cross = (lo_lo >> 32) + (uint32_t)lo_hi + (uint32_t)hi_lo;
    return hi_hi + (lo_hi >> 32) + (hi_lo >> 32) + (cross >> 32);
}

/* RV64M: the multiply/divide group at full 64-bit width (OP-64, funct7 = 0x01).
 * MUL is the low 64 bits; the high-half multiplies use mulhu64 plus the sign
 * corrections (subtract b when a is negative, a when b is negative); divide and
 * remainder mirror RV32M's defined results for /0 and INT64_MIN / -1. */
static void exec_muldiv64(CPU *cpu, uint32_t inst) {
    uint64_t a  = reg_read(cpu, rs1(inst));
    uint64_t b  = reg_read(cpu, rs2(inst));
    int64_t  sa = (int64_t)a, sb = (int64_t)b;
    uint64_t result = 0;

    switch (funct3(inst)) {
        case 0x0: result = a * b; break;                              /* MUL    */
        case 0x1: result = mulhu64(a, b) - (sa < 0 ? b : 0)
                                         - (sb < 0 ? a : 0); break;   /* MULH   */
        case 0x2: result = mulhu64(a, b) - (sa < 0 ? b : 0); break;   /* MULHSU */
        case 0x3: result = mulhu64(a, b); break;                      /* MULHU  */
        case 0x4: /* DIV */
            if (b == 0)                            result = (uint64_t)-1;
            else if (sa == INT64_MIN && sb == -1)  result = (uint64_t)INT64_MIN;
            else                                   result = (uint64_t)(sa / sb);
            break;
        case 0x5: result = (b == 0) ? (uint64_t)-1 : (a / b); break;  /* DIVU */
        case 0x6: /* REM */
            if (b == 0)                            result = a;
            else if (sa == INT64_MIN && sb == -1)  result = 0;
            else                                   result = (uint64_t)(sa % sb);
            break;
        case 0x7: result = (b == 0) ? a : (a % b); break;             /* REMU */
    }
    reg_write(cpu, rd(inst), result);
}

/* Execute RV32M: multiply, divide, and remainder (OP with funct7 = 0x01).
 *
 * RV32M is the first optional *extension* layered on the base integer ISA, so
 * it reuses the OP opcode and is selected purely by funct7. Two cases that
 * fault on many architectures are given defined results here instead of
 * trapping: divide-by-zero, and the signed overflow of INT_MIN / -1. Software
 * tests for them after the fact if it cares.
 *
 * The high-half multiplies form a 64-bit intermediate product; the three
 * variants differ only in whether each operand is sign- or zero-extended. */
static void exec_muldiv(CPU *cpu, uint32_t inst) {
    if (cpu->xlen == 64) { exec_muldiv64(cpu, inst); return; } /* RV64M */
    uint32_t a  = reg_read(cpu, rs1(inst));
    uint32_t b  = reg_read(cpu, rs2(inst));
    int32_t  sa = (int32_t)a, sb = (int32_t)b;
    uint32_t result = 0;

    switch (funct3(inst)) {
        case 0x0: /* MUL: low 32 bits of the product (same bits either signedness) */
            result = (uint32_t)((uint64_t)a * (uint64_t)b);
            break;
        case 0x1: /* MULH:   high 32 bits, signed   x signed   */
            result = (uint32_t)(((int64_t)sa * (int64_t)sb) >> 32);
            break;
        case 0x2: /* MULHSU: high 32 bits, signed   x unsigned */
            result = (uint32_t)(((int64_t)sa * (int64_t)b) >> 32);
            break;
        case 0x3: /* MULHU:  high 32 bits, unsigned x unsigned */
            result = (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32);
            break;
        case 0x4: /* DIV: signed; /0 -> -1, INT_MIN/-1 -> INT_MIN */
            if (b == 0)                           result = 0xffffffffu;
            else if (sa == INT32_MIN && sb == -1) result = (uint32_t)INT32_MIN;
            else                                  result = (uint32_t)(sa / sb);
            break;
        case 0x5: /* DIVU: unsigned; /0 -> all ones */
            result = (b == 0) ? 0xffffffffu : (a / b);
            break;
        case 0x6: /* REM: signed; /0 -> dividend, INT_MIN/-1 -> 0 */
            if (b == 0)                           result = a;
            else if (sa == INT32_MIN && sb == -1) result = 0;
            else                                  result = (uint32_t)(sa % sb);
            break;
        case 0x7: /* REMU: unsigned; /0 -> dividend */
            result = (b == 0) ? a : (a % b);
            break;
    }
    reg_write(cpu, rd(inst), result);
}

/* Execute OP: register/register arithmetic (ADD, SUB, AND, ...). RV32M shares
 * this opcode; funct7 = 0x01 selects its multiply/divide instructions. */
static void exec_op(CPU *cpu, uint32_t inst) {
    if (funct7(inst) == 0x01) { /* RV32M / RV64M extension */
        exec_muldiv(cpu, inst);
        return;
    }
    uint64_t a = reg_read(cpu, rs1(inst));
    uint64_t b = reg_read(cpu, rs2(inst));
    uint32_t shamt = (uint32_t)b & (cpu->xlen == 64 ? 0x3fu : 0x1fu);
    int      alt   = (funct7(inst) == 0x20);   /* SUB (vs ADD), SRA (vs SRL) */
    uint64_t result = 0;

    switch (funct3(inst)) {
        case 0x0: result = alt ? (a - b) : (a + b); break;           /* SUB/ADD */
        case 0x1: result = a << shamt; break;                        /* SLL  */
        case 0x2: result = ((int64_t)a < (int64_t)b) ? 1 : 0; break; /* SLT  */
        case 0x3: result = (a < b) ? 1 : 0; break;                   /* SLTU */
        case 0x4: result = a ^ b; break;                             /* XOR  */
        case 0x5:
            if (alt)                                                 /* SRA  */
                result = cpu->xlen == 64 ? (uint64_t)((int64_t)a >> shamt)
                                         : (uint64_t)((int32_t)a >> shamt);
            else                                                     /* SRL  */
                result = cpu->xlen == 64 ? (a >> shamt)
                                         : ((uint32_t)a >> shamt);
            break;
        case 0x6: result = a | b; break;                             /* OR   */
        case 0x7: result = a & b; break;                             /* AND  */
    }
    reg_write(cpu, rd(inst), result);
}

/* Execute OP-IMM-32 (RV64 only): the *W register/immediate ops — ADDIW and the
 * word shifts SLLIW/SRLIW/SRAIW. Each computes a 32-bit result and sign-extends
 * it to 64 bits. The shamt is always 5 bits here. */
static void exec_op_imm32(CPU *cpu, uint32_t inst) {
    if (cpu->xlen != 64) { raise_trap(cpu, CAUSE_ILLEGAL_INSN, inst); return; }
    uint32_t a     = (uint32_t)reg_read(cpu, rs1(inst));
    int32_t  imm   = imm_i(inst);
    uint32_t shamt = imm & 0x1f;
    uint32_t arith = (inst >> 30) & 1;
    int32_t  result = 0;

    switch (funct3(inst)) {
        case 0x0: result = (int32_t)(a + (uint32_t)imm); break;      /* ADDIW */
        case 0x1: result = (int32_t)(a << shamt); break;             /* SLLIW */
        case 0x5:
            result = arith ? (int32_t)a >> shamt                     /* SRAIW */
                           : (int32_t)((uint32_t)a >> shamt);        /* SRLIW */
            break;
        default: raise_trap(cpu, CAUSE_ILLEGAL_INSN, inst); return;
    }
    reg_write(cpu, rd(inst), (uint64_t)(int64_t)result);
}

/* RV64M *W ops (OP-32, funct7 = 0x01): MULW/DIVW/DIVUW/REMW/REMUW — multiply or
 * divide the low 32 bits and sign-extend the 32-bit result to 64. */
static void exec_muldivw(CPU *cpu, uint32_t inst) {
    uint32_t a  = (uint32_t)reg_read(cpu, rs1(inst));
    uint32_t b  = (uint32_t)reg_read(cpu, rs2(inst));
    int32_t  sa = (int32_t)a, sb = (int32_t)b;
    int32_t  result = 0;

    switch (funct3(inst)) {
        case 0x0: result = (int32_t)(a * b); break;                  /* MULW  */
        case 0x4: /* DIVW */
            if (b == 0)                            result = -1;
            else if (sa == INT32_MIN && sb == -1)  result = INT32_MIN;
            else                                   result = sa / sb;
            break;
        case 0x5: result = (b == 0) ? -1 : (int32_t)(a / b); break;  /* DIVUW */
        case 0x6: /* REMW */
            if (b == 0)                            result = sa;
            else if (sa == INT32_MIN && sb == -1)  result = 0;
            else                                   result = sa % sb;
            break;
        case 0x7: result = (b == 0) ? (int32_t)a : (int32_t)(a % b); break; /* REMUW */
        default: raise_trap(cpu, CAUSE_ILLEGAL_INSN, inst); return;
    }
    reg_write(cpu, rd(inst), (uint64_t)(int64_t)result);
}

/* Execute OP-32 (RV64 only): the *W register/register ops — ADDW/SUBW and the
 * word shifts SLLW/SRLW/SRAW, plus the RV64M *W multiply/divide group. Each
 * produces a 32-bit result, sign-extended to 64. */
static void exec_op32(CPU *cpu, uint32_t inst) {
    if (cpu->xlen != 64) { raise_trap(cpu, CAUSE_ILLEGAL_INSN, inst); return; }
    if (funct7(inst) == 0x01) { exec_muldivw(cpu, inst); return; }
    uint32_t a     = (uint32_t)reg_read(cpu, rs1(inst));
    uint32_t b     = (uint32_t)reg_read(cpu, rs2(inst));
    uint32_t shamt = b & 0x1f;
    int      alt   = (funct7(inst) == 0x20);
    int32_t  result = 0;

    switch (funct3(inst)) {
        case 0x0: result = (int32_t)(alt ? (a - b) : (a + b)); break; /* SUBW/ADDW */
        case 0x1: result = (int32_t)(a << shamt); break;              /* SLLW */
        case 0x5:
            result = alt ? (int32_t)a >> shamt                        /* SRAW */
                         : (int32_t)((uint32_t)a >> shamt);           /* SRLW */
            break;
        default: raise_trap(cpu, CAUSE_ILLEGAL_INSN, inst); return;
    }
    reg_write(cpu, rd(inst), (uint64_t)(int64_t)result);
}

/* Execute BRANCH: conditional PC change. Returns the next PC — the branch target
 * when taken, else the fall-through (pc + the instruction's length, which is 2
 * for a compressed branch and 4 otherwise). */
static uint64_t exec_branch(CPU *cpu, uint32_t inst, uint32_t ilen) {
    /* Compare the full XLEN-wide register values. Registers are stored
     * sign-extended to 64 bits (the Spike convention), so an RV32 comparison is
     * correct too: sign-extension preserves both the signed and the unsigned
     * ordering of the low 32 bits. Truncating to 32 bits here would break RV64
     * branches whose operands differ above bit 31 (e.g. high user VAs). */
    uint64_t a = reg_read(cpu, rs1(inst));
    uint64_t b = reg_read(cpu, rs2(inst));
    int taken = 0;

    switch (funct3(inst)) {
        case 0x0: taken = (a == b); break;                       /* BEQ  */
        case 0x1: taken = (a != b); break;                       /* BNE  */
        case 0x4: taken = ((int64_t)a <  (int64_t)b); break;     /* BLT  */
        case 0x5: taken = ((int64_t)a >= (int64_t)b); break;     /* BGE  */
        case 0x6: taken = (a <  b); break;                       /* BLTU */
        case 0x7: taken = (a >= b); break;                       /* BGEU */
    }
    return taken ? cpu->pc + (uint64_t)imm_b(inst) : cpu->pc + ilen;
}

/* Execute LOAD: read memory into a register. The address is virtual; translate
 * it first, raising a load page fault if the mapping is missing or unreadable. */
static void exec_load(CPU *cpu, uint32_t inst) {
    uint64_t va = reg_read(cpu, rs1(inst)) + (uint64_t)imm_i(inst);
    uint64_t pa;
    uint32_t fault = mmu_translate(cpu, va, ACC_LOAD, &pa);
    if (fault) { raise_trap(cpu, fault, va); return; }
    uint64_t result = 0;

    if (cpu->cache) cache_access(cpu->cache, pa, 0); /* observe, don't alter */

    switch (funct3(inst)) {
        case 0x0: result = (uint64_t)(int64_t)(int8_t) mem_read8 (cpu->mem, pa); break; /* LB  */
        case 0x1: result = (uint64_t)(int64_t)(int16_t)mem_read16(cpu->mem, pa); break; /* LH  */
        case 0x2: result = (uint64_t)(int64_t)(int32_t)mem_read32(cpu->mem, pa); break; /* LW  */
        case 0x3: if (cpu->xlen == 64) result = mem_read64(cpu->mem, pa); break;        /* LD  */
        case 0x4: result = mem_read8 (cpu->mem, pa); break;                             /* LBU */
        case 0x5: result = mem_read16(cpu->mem, pa); break;                             /* LHU */
        case 0x6: if (cpu->xlen == 64) result = mem_read32(cpu->mem, pa); break;        /* LWU */
    }
    reg_write(cpu, rd(inst), result);
}

/* Execute STORE: write a register to memory, translating the virtual address
 * first (a store page fault if the page is missing or read-only). */
static void exec_store(CPU *cpu, uint32_t inst) {
    uint64_t va  = reg_read(cpu, rs1(inst)) + (uint64_t)imm_s(inst);
    uint64_t val = reg_read(cpu, rs2(inst));
    uint64_t pa;
    uint32_t fault = mmu_translate(cpu, va, ACC_STORE, &pa);
    if (fault) { raise_trap(cpu, fault, va); return; }

    break_reservation(cpu, pa); /* RV-A: a plain store can void an LR/SC pair */
    if (cpu->cache) cache_access(cpu->cache, pa, 1); /* observe, don't alter */

    switch (funct3(inst)) {
        case 0x0: mem_write8 (cpu->mem, pa, (uint8_t)val);  break; /* SB */
        case 0x1: mem_write16(cpu->mem, pa, (uint16_t)val); break; /* SH */
        case 0x2: mem_write32(cpu->mem, pa, (uint32_t)val); break; /* SW */
        case 0x3: if (cpu->xlen == 64) mem_write64(cpu->mem, pa, val); break; /* SD */
    }
}

/* ------------------------------------------------------------------------
 * The control/status register file.
 *
 * CSRs occupy a separate 12-bit address space (4096 of them), reached only by
 * the CSR instructions — never by load/store. Most are plain WARL storage; the
 * unprivileged counters (cycle/time/instret) are live views of the retired-
 * instruction count, and the S-mode trap CSRs (sstatus/sie/sip) are masked
 * windows onto their M-mode counterparts rather than registers of their own.
 * csr_read/csr_write are the choke point those special cases — and the
 * privilege and read-only checks in exec_csr — funnel through.
 * ------------------------------------------------------------------------ */

/* Read CSR `addr`. The three unprivileged counters (and their RV32 high
 * halves) read back the retired-instruction count; cycle == instret here
 * because the functional core retires one instruction per "cycle" — the
 * --pipeline overlay is the place that models cycles != instructions. sstatus,
 * sie, and sip return only the S-mode-visible bits of mstatus/mie/mip. */
static uint64_t csr_read(const CPU *cpu, uint32_t addr) {
    switch (addr) {
        case CSR_CYCLE:  case CSR_TIME:  case CSR_INSTRET:
            return cpu->instret; /* RV64: the full counter; RV32: low 32 on write-back */
        case CSR_CYCLEH: case CSR_TIMEH: case CSR_INSTRETH:
            return cpu->instret >> 32; /* RV32 only (illegal on RV64; see exec_csr) */
        case CSR_SSTATUS: return cpu->csr[CSR_MSTATUS] & SSTATUS_MASK;
        case CSR_SIE:     return cpu->csr[CSR_MIE]     & S_INT_MASK;
        case CSR_SIP:     return cpu->csr[CSR_MIP]     & S_INT_MASK;
        default:
            return cpu->csr[addr];
    }
}

/* Write `val` to CSR `addr`. The read-only and privilege checks happen in
 * exec_csr before we get here, so this just commits the bits — writing the
 * S-mode view CSRs back through the masked window onto mstatus/mie/mip. */
static void csr_write(CPU *cpu, uint32_t addr, uint64_t val) {
    switch (addr) {
        case CSR_SSTATUS:
            cpu->csr[CSR_MSTATUS] = (cpu->csr[CSR_MSTATUS] & ~SSTATUS_MASK)
                                  | (val & SSTATUS_MASK);
            return;
        case CSR_SIE:
            cpu->csr[CSR_MIE] = (cpu->csr[CSR_MIE] & ~S_INT_MASK)
                              | (val & S_INT_MASK);
            return;
        case CSR_SIP:
            cpu->csr[CSR_MIP] = (cpu->csr[CSR_MIP] & ~S_INT_MASK)
                              | (val & S_INT_MASK);
            return;
        case CSR_SATP:
            /* satp.MODE is WARL: drop a write selecting a paging scheme we do
             * not model, so a guest probing modes sees it not take. */
            if (!mmu_satp_supported(cpu, val)) return;
            cpu->csr[CSR_SATP] = val;
            mmu_flush(cpu); /* a new address space invalidates cached translations */
            return;
        default:
            cpu->csr[addr] = val;
            return;
    }
}

/* ------------------------------------------------------------------------
 * Traps: the privilege model's control-flow mechanism (M9).
 *
 * An exception (and, once devices exist, an interrupt) suspends the running
 * code and vectors the hart into a handler at a higher or equal privilege. The
 * handler runs with the cause in *cause, the faulting PC in *epc, and any extra
 * detail in *tval; it does its work and returns with mret/sret, which restores
 * the stacked privilege and interrupt-enable state.
 * ------------------------------------------------------------------------ */

/* The built-in supervisor execution environment (SEE): what a trap does when
 * the guest has installed no handler of its own (the target trap vector is 0,
 * its reset value). This is the pre-M9 behaviour — environment calls reach the
 * write/exit syscall layer, and everything else stops the machine — preserved
 * so existing programs keep running until they opt into handling their own
 * traps by setting mtvec/stvec. */
static void legacy_trap(CPU *cpu, uint32_t cause) {
    switch (cause) {
        case CAUSE_ECALL_S:
            /* Supervisor mode talks to the firmware below it via the SBI; with
             * no guest M-mode handler installed, Quanta itself is that firmware
             * (M15). */
            sbi_call(cpu);
            return;
        case CAUSE_ECALL_U: case CAUSE_ECALL_M:
            syscall_dispatch(cpu); /* bare U/M programs use the newlib syscall SEE */
            return;
        case CAUSE_BREAKPOINT:
            cpu->halt_reason = HALT_EBREAK;
            cpu->halted = 1;
            return;
        case CAUSE_INSN_MISALIGNED: case CAUSE_INSN_ACCESS:
        case CAUSE_LOAD_MISALIGNED: case CAUSE_LOAD_ACCESS:
        case CAUSE_STORE_MISALIGNED: case CAUSE_STORE_ACCESS:
            cpu->halt_reason = HALT_MEM_FAULT;
            cpu->halted = 1;
            return;
        default: /* illegal instruction and anything else */
            fprintf(stderr, "trap (cause %u) with no handler at pc=0x%08x\n",
                    cause, (uint32_t)cpu->pc);
            cpu->halt_reason = HALT_ILLEGAL_INSN;
            cpu->halted = 1;
            return;
    }
}

/* Enter a trap handler: the common state-stacking shared by synchronous
 * exceptions and interrupts. `to_s` picks the S- or M-mode CSR set; `cause`
 * already carries its interrupt bit (31) when relevant. The hart saves epc, the
 * cause and tval, stacks the interrupt-enable and previous-privilege bits into
 * mstatus, raises its privilege, and vectors to the handler. Direct mode points
 * every trap at the base; an interrupt under a vectored *tvec (low bit set) goes
 * to base + 4*cause instead. cpu->trapped tells cpu_step the PC is now the
 * handler entry, not the next sequential instruction. */
static void enter_trap(CPU *cpu, uint64_t cause, uint64_t tval, int to_s) {
    uint64_t s = cpu->csr[CSR_MSTATUS];
    uint64_t tvec;
    if (to_s) {
        cpu->csr[CSR_SEPC]   = cpu->pc;
        cpu->csr[CSR_SCAUSE] = cause;
        cpu->csr[CSR_STVAL]  = tval;
        s = (s & ~MSTATUS_SPIE) | ((s & MSTATUS_SIE) ? MSTATUS_SPIE : 0);
        s &= ~MSTATUS_SIE;
        s = (s & ~MSTATUS_SPP) | (cpu->priv ? MSTATUS_SPP : 0); /* prev priv */
        cpu->priv = PRIV_S;
        cpu->csr[CSR_MSTATUS] = s;
        tvec = cpu->csr[CSR_STVEC];
    } else {
        cpu->csr[CSR_MEPC]   = cpu->pc;
        cpu->csr[CSR_MCAUSE] = cause;
        cpu->csr[CSR_MTVAL]  = tval;
        s = (s & ~MSTATUS_MPIE) | ((s & MSTATUS_MIE) ? MSTATUS_MPIE : 0);
        s &= ~MSTATUS_MIE;
        s = (s & ~MSTATUS_MPP) | (cpu->priv << MSTATUS_MPP_SHIFT);
        cpu->priv = PRIV_M;
        cpu->csr[CSR_MSTATUS] = s;
        tvec = cpu->csr[CSR_MTVEC];
    }
    uint64_t intbit = cause_interrupt(cpu);
    uint64_t base = tvec & ~(uint64_t)0x3;
    if ((cause & intbit) && (tvec & 0x1u)) /* vectored mode (interrupts) */
        base += 4u * (cause & ~intbit);
    cpu->pc = base;
    cpu->trapped = 1;
}

/* Raise a synchronous exception with cause `cause` and trap value `tval`.
 *
 * The trap is delegated to S-mode when it arises in S/U mode and the matching
 * medeleg bit is set; otherwise it is taken in M-mode. A trap taken in M never
 * delegates. If the resolved trap vector is still 0, no guest handler exists and
 * we fall back to the built-in SEE; otherwise enter_trap stacks state and
 * vectors into the handler. */
static void raise_trap(CPU *cpu, uint32_t cause, uint64_t tval) {
    int to_s = (cpu->priv <= PRIV_S) &&
               ((cpu->csr[CSR_MEDELEG] >> cause) & 1u);
    uint64_t tvec = to_s ? cpu->csr[CSR_STVEC] : cpu->csr[CSR_MTVEC];

    if ((tvec & ~(uint64_t)0x3) == 0) { /* no handler installed: built-in SEE */
        legacy_trap(cpu, cause);
        return;
    }
    enter_trap(cpu, cause, tval, to_s);
}

/* Refresh the device-driven interrupt-pending bits and return mip. The CLINT and
 * PLIC own MSIP/MTIP/MEIP — they are read-only reflections of device state, so
 * each step we recompute them from the platform rather than trusting a stored
 * value; the software-writable bits (e.g. SSIP) are left as-is. */
static uint32_t effective_mip(CPU *cpu) {
    uint32_t mip = cpu->csr[CSR_MIP];
    if (cpu->mem->plat) {
        /* The PLIC/CLINT own these bits (M/S external via the two PLIC contexts,
         * machine software/timer via the CLINT); recompute them from device
         * state. Software-writable bits (SSIP, STIP) are left as-is. */
        mip &= ~(MIP_MSIP | MIP_MTIP | MIP_MEIP | MIP_SEIP);
        mip |= plat_mip_bits(cpu->mem->plat);
        cpu->csr[CSR_MIP] = mip; /* so a CSR read of mip sees the live bits */
    }
    return mip;
}

/* Deliver a pending interrupt if one is enabled and the privilege gate is open.
 * Returns 1 (and vectors into the handler via enter_trap) when it took one.
 *
 * An interrupt fires when its mip and mie bits are both set and the destination
 * privilege has interrupts globally enabled: M-targeted interrupts when the hart
 * is below M, or in M with mstatus.MIE; S-targeted (delegated via mideleg) when
 * below S, or in S with mstatus.SIE. Among those eligible, the spec's fixed
 * priority order picks one. tval is 0 for interrupts. */
static int take_interrupt(CPU *cpu) {
    uint32_t pending = effective_mip(cpu) & cpu->csr[CSR_MIE];
    if (!pending) return 0;

    uint32_t ms      = cpu->csr[CSR_MSTATUS];
    uint32_t mideleg = cpu->csr[CSR_MIDELEG];
    int m_on = (cpu->priv < PRIV_M) || ((cpu->priv == PRIV_M) && (ms & MSTATUS_MIE));
    int s_on = (cpu->priv < PRIV_S) || ((cpu->priv == PRIV_S) && (ms & MSTATUS_SIE));

    static const uint32_t order[6] = { IRQ_MEXT, IRQ_MSOFT, IRQ_MTIMER,
                                       IRQ_SEXT, IRQ_SSOFT, IRQ_STIMER };
    for (int k = 0; k < 6; k++) {
        uint32_t i = order[k];
        if (!(pending & (1u << i))) continue;
        int to_s = ((mideleg >> i) & 1u) != 0;
        if (to_s ? s_on : m_on) {
            enter_trap(cpu, cause_interrupt(cpu) | i, 0, to_s);
            return 1;
        }
    }
    return 0;
}

/* Arm the supervisor timer for the SBI (see cpu.h). Program the CLINT
 * comparator, clear any pending supervisor timer interrupt (the OS is setting a
 * fresh deadline), and flag it so firmware_timer_tick converts it to STIP once
 * mtime reaches it. */
void cpu_arm_supervisor_timer(CPU *cpu, uint64_t deadline) {
    if (cpu->mem->plat) cpu->mem->plat->clint.mtimecmp = deadline;
    cpu->csr[CSR_MIP] &= ~(1u << IRQ_STIMER);
    cpu->sbi_timer_armed = 1;
}

/* The firmware's job each step: once an SBI-armed deadline is reached (the CLINT
 * asserts MTIP), raise the supervisor timer pending bit (STIP) and disarm — a
 * one-shot the OS re-arms with its next SBI set_timer. This is what M-mode
 * firmware does relaying the machine timer to the supervisor, modelled without a
 * literal trap round-trip: the machine timer is never delivered (an SBI guest
 * leaves mie.MTIE clear), only its STIP shadow, which take_interrupt then routes
 * to S-mode when the OS has delegated and enabled it. */
static void firmware_timer_tick(CPU *cpu) {
    if (!cpu->sbi_timer_armed) return;
    if (plat_mip_bits(cpu->mem->plat) & MIP_MTIP) {
        cpu->csr[CSR_MIP] |= (1u << IRQ_STIMER);
        cpu->sbi_timer_armed = 0;
    }
}

/* menvcfg.STCE (bit 63): enables the Sstc extension's supervisor timer compare. */
#define MENVCFG_STCE ((uint64_t)1 << 63)

/* Sstc: when menvcfg.STCE is set, the supervisor timer interrupt (STIP) is a
 * hardware-driven shadow of stimecmp — pending exactly while time >= stimecmp,
 * with no SBI call or M-mode relay. So each step we recompute STIP directly from
 * stimecmp, overriding software (under Sstc, STIP is read-only to S-mode). This
 * is the path an OS booted without firmware uses (e.g. xv6 with -bios none):
 * writing stimecmp arms the next tick, and it fires when the counter catches up.
 *
 * The counter compared is our `time` CSR, which reads back the retired-instruction
 * count (see csr_read) — so `rdtime` and the deadline stay on one clock. STCE
 * gates the whole thing, so an SBI guest (STCE clear) keeps the firmware_timer_tick
 * path and its own software STIP untouched. */
static void sstc_tick(CPU *cpu) {
    if (!(cpu->csr[CSR_MENVCFG] & MENVCFG_STCE)) return;
    if (cpu->instret >= cpu->csr[CSR_STIMECMP])
        cpu->csr[CSR_MIP] |=  (1u << IRQ_STIMER);
    else
        cpu->csr[CSR_MIP] &= ~(1u << IRQ_STIMER);
}

/* Execute a Zicsr instruction: an atomic read-modify-write of one CSR.
 *
 * Every form reads the old CSR value into rd and then updates the CSR; they
 * differ in how the new value is formed and which side effect is skipped:
 *   - CSRRW(I) writes the operand outright, and skips the *read* when rd == x0.
 *   - CSRRS(I) sets the operand's bits and CSRRC(I) clears them; both skip the
 *     *write* when the source is zero, so a bare read causes no write effects.
 * The immediate forms (funct3 1xx) take a 5-bit zero-extended immediate from
 * the rs1 field instead of the register value; in both forms an rs1 field of 0
 * is the "no source" case, so one test covers register and immediate alike.
 *
 * Two checks now gate the access, raising an illegal-instruction trap rather
 * than acting: the access privilege encoded in CSR bits [9:8] must not exceed
 * the current mode, and an instruction that actually writes must not target a
 * read-only CSR (bits [11:10] == 0b11). A CSRRS/CSRRC whose source is x0 does
 * not write, so a bare counter read does not trip the read-only check. */
static void exec_csr(CPU *cpu, uint32_t inst) {
    uint32_t addr = (inst >> 20) & 0xfff;
    uint32_t f3   = funct3(inst);
    uint32_t rs1f = rs1(inst);
    uint64_t src  = (f3 & 0x4) ? rs1f : reg_read(cpu, rs1f);
    int writes = ((f3 & 0x3) == 0x1) || (rs1f != 0);
    int reads  = ((f3 & 0x3) != 0x1) || (rd(inst) != 0);
    uint64_t old = 0;

    /* The RV32-only high halves (counters, mstatush, and the Sstc/env-config high
     * words) do not exist on RV64. */
    int rv32_only_csr = (addr == CSR_CYCLEH || addr == CSR_TIMEH ||
                         addr == CSR_INSTRETH || addr == CSR_MSTATUSH ||
                         addr == CSR_MENVCFGH || addr == CSR_STIMECMPH);

    /* Sstc: stimecmp is accessible from S-mode only when menvcfg.STCE is set
     * (M-mode always reaches it). Without that, S-mode access is illegal. */
    int sstc_denied = (addr == CSR_STIMECMP || addr == CSR_STIMECMPH) &&
                      cpu->priv == PRIV_S &&
                      !(cpu->csr[CSR_MENVCFG] & MENVCFG_STCE);

    if (cpu->priv < ((addr >> 8) & 0x3) ||           /* insufficient privilege */
        (writes && ((addr >> 10) & 0x3) == 0x3) ||   /* write to a read-only CSR */
        (cpu->xlen == 64 && rv32_only_csr) ||        /* RV32-only CSR on RV64 */
        sstc_denied) {                               /* stimecmp without Sstc */
        raise_trap(cpu, CAUSE_ILLEGAL_INSN, inst);
        return;
    }

    if (reads) old = csr_read(cpu, addr);
    if (writes) {
        switch (f3 & 0x3) {
            case 0x1: csr_write(cpu, addr, src);        break; /* CSRRW(I) */
            case 0x2: csr_write(cpu, addr, old | src);  break; /* CSRRS(I) */
            case 0x3: csr_write(cpu, addr, old & ~src); break; /* CSRRC(I) */
        }
    }
    reg_write(cpu, rd(inst), old);
}

/* Execute RV-A: load-reserved / store-conditional and the atomic memory
 * operations (AMO*). funct3 selects the access width — 0x2 is the 32-bit "word"
 * (RV32A/RV64A), 0x3 the 64-bit "doubleword" (RV64A only) — and funct5 (the top
 * five bits) picks the operation. The aq/rl ordering bits below it are no-ops on
 * a single in-order hart: there is no reordering or other agent for them to
 * fence.
 *
 * An AMO atomically loads at rs1, combines with rs2, and stores the result back,
 * returning the *old* value in rd — a read-modify-write that no sequence of base
 * loads/stores can do indivisibly. On a .W AMO in RV64 the returned old value is
 * sign-extended. LR/SC split that across two instructions: LR loads and registers
 * a reservation, SC stores only if the reservation still holds (0 on success, 1
 * on failure). Atomics must be naturally aligned, so a misaligned address faults
 * rather than being silently handled the way base accesses are. */
static void exec_amo(CPU *cpu, uint32_t inst) {
    uint32_t f3 = funct3(inst);
    int is_d = (f3 == 0x3);
    if (f3 != 0x2 && !(is_d && cpu->xlen == 64)) { /* .W always; .D only on RV64 */
        raise_trap(cpu, CAUSE_ILLEGAL_INSN, inst);
        return;
    }
    uint32_t funct5 = inst >> 27;
    uint64_t va     = reg_read(cpu, rs1(inst));

    if (va & (is_d ? 0x7u : 0x3u)) { /* atomics require natural alignment */
        raise_trap(cpu, (funct5 == 0x02) ? CAUSE_LOAD_MISALIGNED
                                          : CAUSE_STORE_MISALIGNED, va);
        return;
    }
    /* Translate once: LR reads (sets A), SC and the AMOs write (set D). */
    uint64_t addr;
    uint32_t fault = mmu_translate(cpu, va,
                                   (funct5 == 0x02) ? ACC_LOAD : ACC_STORE, &addr);
    if (fault) { raise_trap(cpu, fault, va); return; }
    if (cpu->cache) cache_access(cpu->cache, addr, 1); /* observe the access */

    if (funct5 == 0x02) { /* LR.W/LR.D: load and hold a reservation */
        uint64_t v = is_d ? mem_read64(cpu->mem, addr)
                          : (uint64_t)(int64_t)(int32_t)mem_read32(cpu->mem, addr);
        if (cpu->mem->fault) return; /* cpu_step turns the fault into a trap */
        cpu->reserve_valid = 1;
        cpu->reserve_addr  = addr;
        reg_write(cpu, rd(inst), v);
        return;
    }

    if (funct5 == 0x03) { /* SC.W/SC.D: store iff the reservation still holds */
        uint32_t fail = !(cpu->reserve_valid && cpu->reserve_addr == addr);
        if (!fail) {
            if (is_d) mem_write64(cpu->mem, addr, reg_read(cpu, rs2(inst)));
            else      mem_write32(cpu->mem, addr, (uint32_t)reg_read(cpu, rs2(inst)));
            if (cpu->mem->fault) return;
        }
        cpu->reserve_valid = 0;          /* SC always clears the reservation */
        reg_write(cpu, rd(inst), fail);  /* 0 = success, 1 = failure */
        return;
    }

    /* AMO*: read the old value, combine with rs2, write the result back. The two
     * widths share the operation set; rd gets the (sign-extended, for .W) old. */
    uint64_t b = reg_read(cpu, rs2(inst));
    uint64_t old;
    if (is_d) {
        old = mem_read64(cpu->mem, addr);
        if (cpu->mem->fault) return;
        uint64_t res;
        switch (funct5) {
            case 0x01: res = b;                                     break; /* SWAP */
            case 0x00: res = old + b;                               break; /* ADD  */
            case 0x04: res = old ^ b;                               break; /* XOR  */
            case 0x0c: res = old & b;                               break; /* AND  */
            case 0x08: res = old | b;                               break; /* OR   */
            case 0x10: res = ((int64_t)old < (int64_t)b) ? old : b; break; /* MIN  */
            case 0x14: res = ((int64_t)old > (int64_t)b) ? old : b; break; /* MAX  */
            case 0x18: res = (old < b) ? old : b;                   break; /* MINU */
            case 0x1c: res = (old > b) ? old : b;                   break; /* MAXU */
            default: raise_trap(cpu, CAUSE_ILLEGAL_INSN, inst); return;
        }
        mem_write64(cpu->mem, addr, res);
    } else {
        uint32_t o = mem_read32(cpu->mem, addr);
        if (cpu->mem->fault) return;
        uint32_t bb = (uint32_t)b, r;
        switch (funct5) {
            case 0x01: r = bb;                                      break; /* SWAP */
            case 0x00: r = o + bb;                                  break; /* ADD  */
            case 0x04: r = o ^ bb;                                  break; /* XOR  */
            case 0x0c: r = o & bb;                                  break; /* AND  */
            case 0x08: r = o | bb;                                  break; /* OR   */
            case 0x10: r = ((int32_t)o < (int32_t)bb) ? o : bb;     break; /* MIN  */
            case 0x14: r = ((int32_t)o > (int32_t)bb) ? o : bb;     break; /* MAX  */
            case 0x18: r = (o < bb) ? o : bb;                       break; /* MINU */
            case 0x1c: r = (o > bb) ? o : bb;                       break; /* MAXU */
            default: raise_trap(cpu, CAUSE_ILLEGAL_INSN, inst); return;
        }
        mem_write32(cpu->mem, addr, r);
        old = (uint64_t)(int64_t)(int32_t)o; /* .W returns the sign-extended old */
    }
    if (cpu->mem->fault) return;
    break_reservation(cpu, addr); /* the write voids a reservation on this word */
    reg_write(cpu, rd(inst), old);
}

/* Return from an M-mode trap (MRET). Pop the stacked state mstatus saved on
 * entry: privilege returns to MPP, MIE is restored from MPIE, MPIE is set, and
 * MPP is reset to the least-privileged mode (U). Returns the PC to resume at —
 * the mepc the handler may have advanced past the trapping instruction. */
static uint64_t exec_mret(CPU *cpu) {
    if (cpu->priv < PRIV_M) { raise_trap(cpu, CAUSE_ILLEGAL_INSN, 0); return cpu->pc; }
    uint64_t s = cpu->csr[CSR_MSTATUS];
    uint32_t mpp = (s & MSTATUS_MPP) >> MSTATUS_MPP_SHIFT;
    s = (s & ~MSTATUS_MIE) | ((s & MSTATUS_MPIE) ? MSTATUS_MIE : 0); /* MIE=MPIE */
    s |= MSTATUS_MPIE;                                               /* MPIE=1   */
    s &= ~MSTATUS_MPP;                                               /* MPP=U    */
    if (mpp != PRIV_M) s &= ~MSTATUS_MPRV; /* leaving M clears MPRV */
    cpu->csr[CSR_MSTATUS] = s;
    cpu->priv = mpp;
    return cpu->csr[CSR_MEPC];
}

/* Return from an S-mode trap (SRET). The S-mode mirror of MRET: privilege
 * returns to SPP (U or S), SIE is restored from SPIE, SPIE is set, SPP resets
 * to U. */
static uint64_t exec_sret(CPU *cpu) {
    if (cpu->priv < PRIV_S) { raise_trap(cpu, CAUSE_ILLEGAL_INSN, 0); return cpu->pc; }
    uint64_t s = cpu->csr[CSR_MSTATUS];
    uint32_t spp = (s & MSTATUS_SPP) ? PRIV_S : PRIV_U;
    s = (s & ~MSTATUS_SIE) | ((s & MSTATUS_SPIE) ? MSTATUS_SIE : 0); /* SIE=SPIE */
    s |= MSTATUS_SPIE;                                               /* SPIE=1   */
    s &= ~MSTATUS_SPP;                                               /* SPP=U    */
    s &= ~MSTATUS_MPRV;                  /* returning below M clears MPRV */
    cpu->csr[CSR_MSTATUS] = s;
    cpu->priv = spp;
    return cpu->csr[CSR_SEPC];
}

/* Execute SYSTEM: environment calls, trap returns, and CSR access. Returns the
 * next PC (like exec_branch); a trap raised here instead sets cpu->trapped and
 * cpu->pc, and cpu_step ignores the returned value.
 *
 * With funct3 == 0 the 12-bit immediate selects the operation: ECALL and
 * EBREAK raise their exceptions, MRET/SRET return from a trap, WFI parks the
 * hart (a nop here — a single hart with no interrupt sources has nothing to
 * wait for), and any other encoding is illegal. A non-zero funct3 (other than
 * the reserved 4) is the Zicsr group. */
static uint64_t exec_system(CPU *cpu, uint32_t inst) {
    uint32_t f3 = funct3(inst);

    if (f3 == 0) {
        if ((inst >> 25) == 0x09) { /* SFENCE.VMA (funct7 = 0x09) */
            if (cpu->priv < PRIV_S) raise_trap(cpu, CAUSE_ILLEGAL_INSN, inst);
            else mmu_flush(cpu); /* drop cached translations; the walk is canonical */
            return cpu->pc + 4;
        }
        switch (inst >> 20) { /* funct12 */
            case 0x000: /* ECALL: environment call from the current privilege */
                raise_trap(cpu, CAUSE_ECALL_U + cpu->priv, 0);
                return cpu->pc + 4; /* used only if the SEE serviced it inline */
            case 0x001: /* EBREAK */
                raise_trap(cpu, CAUSE_BREAKPOINT, 0);
                return cpu->pc + 4;
            case 0x302: return exec_mret(cpu); /* MRET */
            case 0x102: return exec_sret(cpu); /* SRET */
            case 0x105: return cpu->pc + 4;    /* WFI: nop */
            default:
                raise_trap(cpu, CAUSE_ILLEGAL_INSN, inst);
                return cpu->pc + 4;
        }
    }

    if (f3 == 0x4) { /* reserved CSR funct3 */
        raise_trap(cpu, CAUSE_ILLEGAL_INSN, inst);
        return cpu->pc + 4;
    }

    exec_csr(cpu, inst); /* Zicsr; may itself raise an illegal-instruction trap */
    return cpu->pc + 4;
}

void cpu_step(CPU *cpu) {
    /* Clear any stale fault; a faulting access this step sets it and we turn
     * that into a trap rather than letting the access abort the host. */
    cpu->mem->fault = 0;
    cpu->trapped = 0; /* set by raise_trap if this step vectors into a handler */

    /* Advance the platform timer and take any pending interrupt before fetch,
     * at this instruction boundary. A taken interrupt vectors into the handler
     * and retires nothing, so we return without fetching. */
    if (cpu->mem->plat) {
        plat_tick(cpu->mem->plat);
        firmware_timer_tick(cpu); /* convert a reached SBI deadline into STIP */
        sstc_tick(cpu);           /* or drive STIP directly from stimecmp (Sstc) */
        if (take_interrupt(cpu)) return;
    }

    /* FETCH: read the low halfword first. Its low two bits decide the length:
     * a value other than 0b11 is a 16-bit compressed instruction (RV32C),
     * expanded here to the 32-bit instruction it stands for; 0b11 introduces a
     * 32-bit instruction whose upper halfword may lie in the next page, so it is
     * translated separately. Either way decode/execute below is unchanged. */
    uint64_t lo_pa;
    uint32_t fc = mmu_translate(cpu, cpu->pc, ACC_FETCH, &lo_pa);
    if (fc) { raise_trap(cpu, fc, cpu->pc); return; } /* instruction page fault */
    uint16_t lo = mem_read16(cpu->mem, lo_pa);
    if (cpu->mem->fault) { /* unmapped fetch: trap before decoding garbage */
        raise_trap(cpu, CAUSE_INSN_ACCESS, cpu->pc);
        return;
    }

    uint32_t inst, ilen;
    if ((lo & 0x3u) != 0x3u) {           /* 16-bit compressed instruction */
        inst = rvc_expand(lo, cpu->xlen == 64);
        ilen = 2;
        if (inst == RVC_ILLEGAL) { raise_trap(cpu, CAUSE_ILLEGAL_INSN, lo); return; }
    } else {                             /* 32-bit instruction: fetch upper half */
        uint64_t hi_pa;
        uint32_t fch = mmu_translate(cpu, cpu->pc + 2, ACC_FETCH, &hi_pa);
        if (fch) { raise_trap(cpu, fch, cpu->pc + 2); return; }
        uint16_t hi = mem_read16(cpu->mem, hi_pa);
        if (cpu->mem->fault) { raise_trap(cpu, CAUSE_INSN_ACCESS, cpu->pc + 2); return; }
        inst = (uint32_t)lo | ((uint32_t)hi << 16);
        ilen = 4;
    }
    uint64_t next_pc = cpu->pc + ilen; /* default: fall through to the next insn */

    /* DECODE + EXECUTE: dispatch on the opcode field. */
    switch (opcode(inst)) {
        case OP_IMM:
            exec_op_imm(cpu, inst);
            break;

        case OP_IMM_32:
            exec_op_imm32(cpu, inst); /* RV64 ADDIW/SLLIW/SRLIW/SRAIW */
            break;

        case OP_REG:
            exec_op(cpu, inst);
            break;

        case OP_REG_32:
            exec_op32(cpu, inst);     /* RV64 ADDW/SUBW + W ops */
            break;

        case OP_LUI: /* load upper immediate (sign-extended to XLEN) */
            reg_write(cpu, rd(inst), (uint64_t)imm_u(inst));
            break;

        case OP_AUIPC: /* add upper immediate to PC */
            reg_write(cpu, rd(inst), cpu->pc + (uint64_t)imm_u(inst));
            break;

        case OP_JAL: /* jump and link (link address is the next instruction) */
            reg_write(cpu, rd(inst), cpu->pc + ilen);
            next_pc = cpu->pc + (uint64_t)imm_j(inst);
            break;

        case OP_JALR: /* jump and link register */
            reg_write(cpu, rd(inst), cpu->pc + ilen);
            /* target = (rs1 + imm) with the low bit cleared */
            next_pc = (reg_read(cpu, rs1(inst)) + (uint64_t)imm_i(inst)) & ~(uint64_t)1;
            break;

        case OP_BRANCH:
            next_pc = exec_branch(cpu, inst, ilen);
            break;

        case OP_LOAD:
            exec_load(cpu, inst);
            break;

        case OP_STORE:
            exec_store(cpu, inst);
            break;

        case OP_AMO:
            exec_amo(cpu, inst);
            break;

        case OP_FENCE:
            /* FENCE / FENCE.I: memory- and instruction-ordering hints. A single
             * hart that executes in program order, with no modelled instruction
             * cache, has nothing to reorder, so these are no-ops (PC += 4). */
            break;

        case OP_SYSTEM:
            next_pc = exec_system(cpu, inst);
            break;

        default:
            raise_trap(cpu, CAUSE_ILLEGAL_INSN, inst);
            break;
    }

    if (cpu->mem->fault) { /* a load, store, or atomic left the mapped region */
        uint32_t op = opcode(inst);
        uint32_t cause = (op == OP_STORE || op == OP_AMO) ? CAUSE_STORE_ACCESS
                                                          : CAUSE_LOAD_ACCESS;
        raise_trap(cpu, cause, cpu->mem->fault_addr);
        return; /* don't commit PC past the faulting instruction */
    }

    if (cpu->trapped) return; /* a trap redirected the PC; the insn didn't retire */
    if (cpu->halted)  return; /* the built-in SEE stopped the machine */

    if (next_pc & 0x1) { /* control transfer to a misaligned target (IALIGN=16
                          * with RV32C: only an odd target is misaligned) */
        raise_trap(cpu, CAUSE_INSN_MISALIGNED, next_pc);
        return;
    }

    cpu->instret++; /* the instruction retired; drives the counter CSRs */
    cpu->pc = sext_xlen(cpu, next_pc); /* keep the sext invariant on PC */
}

const char *halt_reason_str(HaltReason r) {
    switch (r) {
        case HALT_NONE:            return "running";
        case HALT_EXIT:            return "exit syscall";
        case HALT_EBREAK:          return "ebreak";
        case HALT_ILLEGAL_INSN:    return "illegal instruction";
        case HALT_UNIMP_SYSTEM:    return "unimplemented system instruction";
        case HALT_UNKNOWN_SYSCALL: return "unknown syscall";
        case HALT_MEM_FAULT:       return "memory access out of range";
    }
    return "unknown";
}
