#include "cpu.h"
#include "decode.h"
#include "syscall.h"
#include "cache.h"

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

/* ------------------------------------------------------------------------
 * Privileged-architecture constants (M9).
 *
 * mstatus packs the trap-handling state into named bit fields; a trap stacks
 * the interrupt-enable and previous-privilege bits here and mret/sret pop them.
 * The S-mode CSRs (sstatus/sie/sip) are not separate registers but masked
 * windows onto mstatus/mie/mip, so the masks below pick out the bits S-mode is
 * allowed to see and change.
 * ------------------------------------------------------------------------ */
enum {
    MSTATUS_SIE  = 1u << 1,   /* S-mode interrupt enable            */
    MSTATUS_MIE  = 1u << 3,   /* M-mode interrupt enable            */
    MSTATUS_SPIE = 1u << 5,   /* previous SIE, stacked on an S-trap */
    MSTATUS_MPIE = 1u << 7,   /* previous MIE, stacked on an M-trap */
    MSTATUS_SPP  = 1u << 8,   /* previous privilege for S (1 bit)   */
    MSTATUS_MPP  = 3u << 11,  /* previous privilege for M (2 bits)  */
    MSTATUS_MPRV = 1u << 17,  /* load/store as MPP (inert until paging, M12) */
    MSTATUS_SUM  = 1u << 18,
    MSTATUS_MXR  = 1u << 19,
    MSTATUS_MPP_SHIFT = 11,

    /* sstatus sees these mstatus bits; everything else reads 0 / ignores writes.
     * (FS/XS/SD are float/extension state we don't model yet.) */
    SSTATUS_MASK = MSTATUS_SIE | MSTATUS_SPIE | MSTATUS_SPP |
                   MSTATUS_SUM | MSTATUS_MXR,

    /* sie/sip see only the S-mode interrupt bits (software/timer/external). */
    S_INT_MASK = (1u << 1) | (1u << 5) | (1u << 9)
};

/* Synchronous exception causes (mcause/scause with the interrupt bit clear).
 * The ecall causes are contiguous from U so `ECALL_U + priv` selects the right
 * one (priv is 0/1/3, and there is deliberately no cause 10). */
enum {
    CAUSE_INSN_MISALIGNED = 0,
    CAUSE_INSN_ACCESS     = 1,
    CAUSE_ILLEGAL_INSN    = 2,
    CAUSE_BREAKPOINT      = 3,
    CAUSE_LOAD_MISALIGNED = 4,
    CAUSE_LOAD_ACCESS     = 5,
    CAUSE_STORE_MISALIGNED= 6,
    CAUSE_STORE_ACCESS    = 7,
    CAUSE_ECALL_U         = 8,
    CAUSE_ECALL_S         = 9,
    CAUSE_ECALL_M         = 11
};

void cpu_init(CPU *cpu, Memory *mem, uint32_t entry_pc) {
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
    memset(cpu->csr, 0, sizeof cpu->csr);
    /* misa advertises the base and extensions: MXL=1 (RV32), I, M, S, U. It is
     * informational here; reads see this, writes are accepted as WARL storage. */
    cpu->csr[CSR_MISA] = (1u << 30) | (1u << 8) | (1u << 12) |
                         (1u << 18) | (1u << 20);
}

uint32_t reg_read(const CPU *cpu, uint32_t i) {
    return (i == 0) ? 0u : cpu->regs[i];
}

void reg_write(CPU *cpu, uint32_t i, uint32_t value) {
    if (i != 0) cpu->regs[i] = value; /* x0 stays zero */
}

/* Execute OP-IMM: register/immediate arithmetic (ADDI, SLTI, shifts, ...). */
static void exec_op_imm(CPU *cpu, uint32_t inst) {
    uint32_t a    = reg_read(cpu, rs1(inst));
    int32_t  imm  = imm_i(inst);
    uint32_t shamt = imm & 0x1f; /* shift amount is low 5 bits */
    uint32_t result = 0;

    switch (funct3(inst)) {
        case 0x0: result = a + (uint32_t)imm; break;                 /* ADDI  */
        case 0x2: result = ((int32_t)a < imm) ? 1 : 0; break;        /* SLTI  */
        case 0x3: result = (a < (uint32_t)imm) ? 1 : 0; break;       /* SLTIU */
        case 0x4: result = a ^ (uint32_t)imm; break;                 /* XORI  */
        case 0x6: result = a | (uint32_t)imm; break;                 /* ORI   */
        case 0x7: result = a & (uint32_t)imm; break;                 /* ANDI  */
        case 0x1: result = a << shamt; break;                        /* SLLI  */
        case 0x5:
            if (funct7(inst) == 0x20)
                result = (uint32_t)((int32_t)a >> shamt);            /* SRAI  */
            else
                result = a >> shamt;                                 /* SRLI  */
            break;
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
    if (funct7(inst) == 0x01) { /* RV32M extension */
        exec_muldiv(cpu, inst);
        return;
    }
    uint32_t a = reg_read(cpu, rs1(inst));
    uint32_t b = reg_read(cpu, rs2(inst));
    uint32_t shamt = b & 0x1f;
    uint32_t result = 0;

    switch (funct3(inst)) {
        case 0x0:
            result = (funct7(inst) == 0x20) ? (a - b) : (a + b);     /* SUB/ADD */
            break;
        case 0x1: result = a << shamt; break;                        /* SLL  */
        case 0x2: result = ((int32_t)a < (int32_t)b) ? 1 : 0; break; /* SLT  */
        case 0x3: result = (a < b) ? 1 : 0; break;                   /* SLTU */
        case 0x4: result = a ^ b; break;                             /* XOR  */
        case 0x5:
            if (funct7(inst) == 0x20)
                result = (uint32_t)((int32_t)a >> shamt);            /* SRA  */
            else
                result = a >> shamt;                                 /* SRL  */
            break;
        case 0x6: result = a | b; break;                             /* OR   */
        case 0x7: result = a & b; break;                             /* AND  */
    }
    reg_write(cpu, rd(inst), result);
}

/* Execute BRANCH: conditional PC change. Returns the next PC. */
static uint32_t exec_branch(CPU *cpu, uint32_t inst) {
    uint32_t a = reg_read(cpu, rs1(inst));
    uint32_t b = reg_read(cpu, rs2(inst));
    int taken = 0;

    switch (funct3(inst)) {
        case 0x0: taken = (a == b); break;                       /* BEQ  */
        case 0x1: taken = (a != b); break;                       /* BNE  */
        case 0x4: taken = ((int32_t)a <  (int32_t)b); break;     /* BLT  */
        case 0x5: taken = ((int32_t)a >= (int32_t)b); break;     /* BGE  */
        case 0x6: taken = (a <  b); break;                       /* BLTU */
        case 0x7: taken = (a >= b); break;                       /* BGEU */
    }
    return taken ? cpu->pc + (uint32_t)imm_b(inst) : cpu->pc + 4;
}

/* Execute LOAD: read memory into a register. */
static void exec_load(CPU *cpu, uint32_t inst) {
    uint32_t addr = reg_read(cpu, rs1(inst)) + (uint32_t)imm_i(inst);
    uint32_t result = 0;

    if (cpu->cache) cache_access(cpu->cache, addr, 0); /* observe, don't alter */

    switch (funct3(inst)) {
        case 0x0: result = (uint32_t)(int8_t)mem_read8(cpu->mem, addr); break;  /* LB  */
        case 0x1: result = (uint32_t)(int16_t)mem_read16(cpu->mem, addr); break;/* LH  */
        case 0x2: result = mem_read32(cpu->mem, addr); break;                   /* LW  */
        case 0x4: result = mem_read8(cpu->mem, addr); break;                    /* LBU */
        case 0x5: result = mem_read16(cpu->mem, addr); break;                   /* LHU */
    }
    reg_write(cpu, rd(inst), result);
}

/* Execute STORE: write a register to memory. */
static void exec_store(CPU *cpu, uint32_t inst) {
    uint32_t addr = reg_read(cpu, rs1(inst)) + (uint32_t)imm_s(inst);
    uint32_t val  = reg_read(cpu, rs2(inst));

    if (cpu->cache) cache_access(cpu->cache, addr, 1); /* observe, don't alter */

    switch (funct3(inst)) {
        case 0x0: mem_write8 (cpu->mem, addr, (uint8_t)val);  break; /* SB */
        case 0x1: mem_write16(cpu->mem, addr, (uint16_t)val); break; /* SH */
        case 0x2: mem_write32(cpu->mem, addr, val);           break; /* SW */
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
static uint32_t csr_read(const CPU *cpu, uint32_t addr) {
    switch (addr) {
        case CSR_CYCLE:  case CSR_TIME:  case CSR_INSTRET:
            return (uint32_t)cpu->instret;
        case CSR_CYCLEH: case CSR_TIMEH: case CSR_INSTRETH:
            return (uint32_t)(cpu->instret >> 32);
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
static void csr_write(CPU *cpu, uint32_t addr, uint32_t val) {
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
        case CAUSE_ECALL_U: case CAUSE_ECALL_S: case CAUSE_ECALL_M:
            syscall_dispatch(cpu); /* may set a0, or halt on exit */
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
                    cause, cpu->pc);
            cpu->halt_reason = HALT_ILLEGAL_INSN;
            cpu->halted = 1;
            return;
    }
}

/* Raise a synchronous exception with cause `cause` and trap value `tval`.
 *
 * The trap is delegated to S-mode when it arises in S/U mode and the matching
 * medeleg bit is set; otherwise it is taken in M-mode. A trap taken in M never
 * delegates. If the resolved trap vector is still 0, no guest handler exists
 * and we fall back to the built-in SEE. Otherwise we save the architectural
 * state the spec requires, stack the interrupt-enable and previous-privilege
 * bits, and vector to the handler. cpu->trapped tells cpu_step the PC is now
 * the handler entry, not the next sequential instruction. */
static void raise_trap(CPU *cpu, uint32_t cause, uint32_t tval) {
    int to_s = (cpu->priv <= PRIV_S) &&
               ((cpu->csr[CSR_MEDELEG] >> cause) & 1u);
    uint32_t tvec = to_s ? cpu->csr[CSR_STVEC] : cpu->csr[CSR_MTVEC];

    if ((tvec & ~0x3u) == 0) { /* no handler installed: built-in SEE */
        legacy_trap(cpu, cause);
        return;
    }

    uint32_t s = cpu->csr[CSR_MSTATUS];
    if (to_s) {
        cpu->csr[CSR_SEPC]   = cpu->pc;
        cpu->csr[CSR_SCAUSE] = cause; /* interrupt bit (31) stays 0 */
        cpu->csr[CSR_STVAL]  = tval;
        s = (s & ~MSTATUS_SPIE) | ((s & MSTATUS_SIE) ? MSTATUS_SPIE : 0);
        s &= ~MSTATUS_SIE;
        s = (s & ~MSTATUS_SPP) | (cpu->priv ? MSTATUS_SPP : 0); /* prev priv */
        cpu->priv = PRIV_S;
    } else {
        cpu->csr[CSR_MEPC]   = cpu->pc;
        cpu->csr[CSR_MCAUSE] = cause;
        cpu->csr[CSR_MTVAL]  = tval;
        s = (s & ~MSTATUS_MPIE) | ((s & MSTATUS_MIE) ? MSTATUS_MPIE : 0);
        s &= ~MSTATUS_MIE;
        s = (s & ~MSTATUS_MPP) | (cpu->priv << MSTATUS_MPP_SHIFT);
        cpu->priv = PRIV_M;
    }
    cpu->csr[CSR_MSTATUS] = s;
    cpu->pc = tvec & ~0x3u; /* direct mode; vectored is for interrupts (M13) */
    cpu->trapped = 1;
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
    uint32_t src  = (f3 & 0x4) ? rs1f : reg_read(cpu, rs1f);
    int writes = ((f3 & 0x3) == 0x1) || (rs1f != 0);
    int reads  = ((f3 & 0x3) != 0x1) || (rd(inst) != 0);
    uint32_t old = 0;

    if (cpu->priv < ((addr >> 8) & 0x3) ||           /* insufficient privilege */
        (writes && ((addr >> 10) & 0x3) == 0x3)) {   /* write to a read-only CSR */
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

/* Return from an M-mode trap (MRET). Pop the stacked state mstatus saved on
 * entry: privilege returns to MPP, MIE is restored from MPIE, MPIE is set, and
 * MPP is reset to the least-privileged mode (U). Returns the PC to resume at —
 * the mepc the handler may have advanced past the trapping instruction. */
static uint32_t exec_mret(CPU *cpu) {
    if (cpu->priv < PRIV_M) { raise_trap(cpu, CAUSE_ILLEGAL_INSN, 0); return cpu->pc; }
    uint32_t s = cpu->csr[CSR_MSTATUS];
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
static uint32_t exec_sret(CPU *cpu) {
    if (cpu->priv < PRIV_S) { raise_trap(cpu, CAUSE_ILLEGAL_INSN, 0); return cpu->pc; }
    uint32_t s = cpu->csr[CSR_MSTATUS];
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
static uint32_t exec_system(CPU *cpu, uint32_t inst) {
    uint32_t f3 = funct3(inst);

    if (f3 == 0) {
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

    /* FETCH: read the 32-bit instruction word at PC. */
    uint32_t inst = mem_read32(cpu->mem, cpu->pc);
    if (cpu->mem->fault) { /* unmapped fetch: trap before decoding garbage */
        raise_trap(cpu, CAUSE_INSN_ACCESS, cpu->pc);
        return;
    }
    uint32_t next_pc = cpu->pc + 4; /* default: fall through to next word */

    /* DECODE + EXECUTE: dispatch on the opcode field. */
    switch (opcode(inst)) {
        case OP_IMM:
            exec_op_imm(cpu, inst);
            break;

        case OP_REG:
            exec_op(cpu, inst);
            break;

        case OP_LUI: /* load upper immediate */
            reg_write(cpu, rd(inst), (uint32_t)imm_u(inst));
            break;

        case OP_AUIPC: /* add upper immediate to PC */
            reg_write(cpu, rd(inst), cpu->pc + (uint32_t)imm_u(inst));
            break;

        case OP_JAL: /* jump and link */
            reg_write(cpu, rd(inst), cpu->pc + 4);
            next_pc = cpu->pc + (uint32_t)imm_j(inst);
            break;

        case OP_JALR: /* jump and link register */
            reg_write(cpu, rd(inst), cpu->pc + 4);
            /* target = (rs1 + imm) with the low bit cleared */
            next_pc = (reg_read(cpu, rs1(inst)) + (uint32_t)imm_i(inst)) & ~1u;
            break;

        case OP_BRANCH:
            next_pc = exec_branch(cpu, inst);
            break;

        case OP_LOAD:
            exec_load(cpu, inst);
            break;

        case OP_STORE:
            exec_store(cpu, inst);
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

    if (cpu->mem->fault) { /* a load or store left the mapped region */
        uint32_t cause = (opcode(inst) == OP_STORE) ? CAUSE_STORE_ACCESS
                                                     : CAUSE_LOAD_ACCESS;
        raise_trap(cpu, cause, cpu->mem->fault_addr);
        return; /* don't commit PC past the faulting instruction */
    }

    if (cpu->trapped) return; /* a trap redirected the PC; the insn didn't retire */
    if (cpu->halted)  return; /* the built-in SEE stopped the machine */

    if (next_pc & 0x3) { /* control transfer to a misaligned target */
        raise_trap(cpu, CAUSE_INSN_MISALIGNED, next_pc);
        return;
    }

    cpu->instret++; /* the instruction retired; drives the counter CSRs */
    cpu->pc = next_pc;
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
