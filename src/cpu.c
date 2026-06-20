#include "cpu.h"
#include "decode.h"
#include "syscall.h"

#include <stdio.h>

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
    cpu->halted    = 0;
    cpu->exited    = 0;
    cpu->exit_code = 0;
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

/* Execute OP: register/register arithmetic (ADD, SUB, AND, ...). */
static void exec_op(CPU *cpu, uint32_t inst) {
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

    switch (funct3(inst)) {
        case 0x0: mem_write8 (cpu->mem, addr, (uint8_t)val);  break; /* SB */
        case 0x1: mem_write16(cpu->mem, addr, (uint16_t)val); break; /* SH */
        case 0x2: mem_write32(cpu->mem, addr, val);           break; /* SW */
    }
}

/* Execute SYSTEM: environment calls. ECALL traps to the syscall layer; EBREAK
 * stops the machine (a breakpoint with no debugger attached). The two differ
 * only in the 12-bit immediate (funct12). CSR instructions (funct3 != 0) are
 * not modelled yet. */
static void exec_system(CPU *cpu, uint32_t inst) {
    uint32_t funct12 = inst >> 20;
    if (funct3(inst) == 0) {
        if (funct12 == 0x000) { /* ECALL  */
            syscall_dispatch(cpu);
            return;
        }
        if (funct12 == 0x001) { /* EBREAK */
            cpu->halted = 1;
            return;
        }
    }
    fprintf(stderr, "unimplemented SYSTEM instruction 0x%08x at pc=0x%08x\n",
            inst, cpu->pc);
    cpu->halted = 1;
}

void cpu_step(CPU *cpu) {
    /* FETCH: read the 32-bit instruction word at PC. */
    uint32_t inst = mem_read32(cpu->mem, cpu->pc);
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
            cpu->halted = 1;
            break;
    }

    cpu->pc = next_pc;
}

void cpu_dump(const CPU *cpu) {
    /* ABI register names (decode.h) make the dump easy to cross-check against
     * a disassembly. */
    printf("pc = 0x%08x\n", cpu->pc);
    for (int i = 0; i < 32; i++) {
        printf("x%-2d %-4s = 0x%08x", i, reg_abi_name((uint32_t)i), cpu->regs[i]);
        printf((i % 2) ? "\n" : "    ");
    }
    if (32 % 2) printf("\n");
}
