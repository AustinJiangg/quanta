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

void cpu_init(CPU *cpu, Memory *mem, uint32_t entry_pc) {
    for (int i = 0; i < 32; i++) cpu->regs[i] = 0;
    cpu->pc        = entry_pc;
    cpu->mem       = mem;
    cpu->cache     = NULL;
    cpu->halted      = 0;
    cpu->halt_reason = HALT_NONE;
    cpu->exit_code   = 0;
    cpu->instret     = 0;
    memset(cpu->csr, 0, sizeof cpu->csr);
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
 * Zicsr: the control/status register file.
 *
 * CSRs occupy a separate 12-bit address space (4096 of them), reached only by
 * the CSR instructions — never by load/store. Most are plain WARL storage at
 * this milestone; the unprivileged counters (cycle/time/instret) are live
 * views of the retired-instruction count. The privilege model and the
 * architecturally-defined trap CSRs (mstatus/mtvec/...) arrive in M9, so
 * csr_read/csr_write are deliberately a choke point: that is where privilege
 * checks and read side effects will hook in.
 * ------------------------------------------------------------------------ */

/* Read CSR `addr`. The three unprivileged counters (and their RV32 high
 * halves) read back the retired-instruction count; cycle == instret here
 * because the functional core retires one instruction per "cycle" — the
 * --pipeline overlay is the place that models cycles != instructions. */
static uint32_t csr_read(const CPU *cpu, uint32_t addr) {
    switch (addr) {
        case CSR_CYCLE:  case CSR_TIME:  case CSR_INSTRET:
            return (uint32_t)cpu->instret;
        case CSR_CYCLEH: case CSR_TIMEH: case CSR_INSTRETH:
            return (uint32_t)(cpu->instret >> 32);
        default:
            return cpu->csr[addr];
    }
}

/* Write `val` to CSR `addr`. A CSR whose address has 0b11 in its top two bits
 * is read-only by the spec's encoding convention (the counters live there):
 * drop the write rather than fault, since traps don't exist until M9. */
static void csr_write(CPU *cpu, uint32_t addr, uint32_t val) {
    if (((addr >> 10) & 0x3) == 0x3) return; /* read-only CSR */
    cpu->csr[addr] = val;
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
 * is the "no source" case, so one test covers register and immediate alike. */
static void exec_csr(CPU *cpu, uint32_t inst) {
    uint32_t addr = (inst >> 20) & 0xfff;
    uint32_t f3   = funct3(inst);
    uint32_t rs1f = rs1(inst);
    uint32_t src  = (f3 & 0x4) ? rs1f : reg_read(cpu, rs1f);
    uint32_t old  = 0;

    switch (f3 & 0x3) {
        case 0x1: /* CSRRW(I): swap; the read is suppressed when rd is x0 */
            if (rd(inst) != 0) old = csr_read(cpu, addr);
            csr_write(cpu, addr, src);
            break;
        case 0x2: /* CSRRS(I): read, then set bits (no write if source is 0) */
            old = csr_read(cpu, addr);
            if (rs1f != 0) csr_write(cpu, addr, old | src);
            break;
        case 0x3: /* CSRRC(I): read, then clear bits (no write if source is 0) */
            old = csr_read(cpu, addr);
            if (rs1f != 0) csr_write(cpu, addr, old & ~src);
            break;
    }
    reg_write(cpu, rd(inst), old);
}

/* Execute SYSTEM: environment calls and CSR access. With funct3 == 0, ECALL
 * traps to the syscall layer and EBREAK stops the machine (a breakpoint with
 * no debugger attached) — the two differ only in the 12-bit immediate. A
 * non-zero funct3 (other than the reserved 4) selects the Zicsr instructions.
 * Other funct3 == 0 system ops (mret/sret/wfi) are privileged-spec material
 * for M9 and stop the machine for now. */
static void exec_system(CPU *cpu, uint32_t inst) {
    uint32_t f3 = funct3(inst);

    if (f3 == 0) {
        uint32_t funct12 = inst >> 20;
        if (funct12 == 0x000) { syscall_dispatch(cpu); return; } /* ECALL  */
        if (funct12 == 0x001) {                                  /* EBREAK */
            cpu->halt_reason = HALT_EBREAK;
            cpu->halted = 1;
            return;
        }
    } else if (f3 != 0x4) { /* funct3 1/2/3/5/6/7 = the Zicsr group */
        exec_csr(cpu, inst);
        return;
    }

    fprintf(stderr, "unimplemented SYSTEM instruction 0x%08x at pc=0x%08x\n",
            inst, cpu->pc);
    cpu->halt_reason = HALT_UNIMP_SYSTEM;
    cpu->halted = 1;
}

void cpu_step(CPU *cpu) {
    /* Clear any stale fault; a faulting access this step sets it and we turn
     * that into a halt rather than letting the access abort the host. */
    cpu->mem->fault = 0;

    /* FETCH: read the 32-bit instruction word at PC. */
    uint32_t inst = mem_read32(cpu->mem, cpu->pc);
    if (cpu->mem->fault) { /* unmapped fetch: stop before decoding garbage */
        cpu->halt_reason = HALT_MEM_FAULT;
        cpu->halted = 1;
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
            exec_system(cpu, inst);
            break;

        default:
            fprintf(stderr,
                    "illegal/unimplemented instruction 0x%08x at pc=0x%08x\n",
                    inst, cpu->pc);
            cpu->halt_reason = HALT_ILLEGAL_INSN;
            cpu->halted = 1;
            break;
    }

    if (cpu->mem->fault) { /* a load or store left the mapped region */
        cpu->halt_reason = HALT_MEM_FAULT;
        cpu->halted = 1;
        return; /* don't commit PC past the faulting instruction */
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
