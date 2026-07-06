#include "disasm.h"
#include "decode.h"
#include "rvc.h"

#include <stdio.h>

/*
 * RV32I disassembler — the reverse of cpu.c's decode/execute.
 *
 * The structure mirrors cpu_step(): switch on the opcode, then on funct3/
 * funct7, reusing the same field and immediate helpers from decode.h so the
 * two views can never disagree about how an instruction is laid out.
 *
 * Where a plain encoding has a well-known shorthand (a "pseudo-instruction"),
 * we print that instead, matching what `objdump -d` shows: ADDI with a zero
 * source is `li`, JALR x0,0(ra) is `ret`, and so on. Targets of branches and
 * jumps are rendered as absolute addresses (pc + offset), like objdump.
 *
 * Formatting follows objdump's conventions: ABI register names, signed
 * decimal for arithmetic/offset immediates, and hex (0x..) for shift amounts
 * and the upper-immediate field.
 */

/* objdump prints known CSRs by name; we model the ones the core touches —
 * the unprivileged counters and the M9 machine/supervisor trap registers. */
static const char *csr_name(uint32_t addr) {
    switch (addr) {
        case CSR_CYCLE:    return "cycle";
        case CSR_TIME:     return "time";
        case CSR_INSTRET:  return "instret";
        case CSR_CYCLEH:   return "cycleh";
        case CSR_TIMEH:    return "timeh";
        case CSR_INSTRETH: return "instreth";
        /* Supervisor trap CSRs. */
        case CSR_SSTATUS:    return "sstatus";
        case CSR_SIE:        return "sie";
        case CSR_STVEC:      return "stvec";
        case CSR_SCOUNTEREN: return "scounteren";
        case CSR_SSCRATCH:   return "sscratch";
        case CSR_SEPC:       return "sepc";
        case CSR_SCAUSE:     return "scause";
        case CSR_STVAL:      return "stval";
        case CSR_SIP:        return "sip";
        case CSR_SATP:       return "satp";
        /* Machine information and trap CSRs. */
        case CSR_MVENDORID:  return "mvendorid";
        case CSR_MARCHID:    return "marchid";
        case CSR_MIMPID:     return "mimpid";
        case CSR_MHARTID:    return "mhartid";
        case CSR_MSTATUS:    return "mstatus";
        case CSR_MISA:       return "misa";
        case CSR_MEDELEG:    return "medeleg";
        case CSR_MIDELEG:    return "mideleg";
        case CSR_MIE:        return "mie";
        case CSR_MTVEC:      return "mtvec";
        case CSR_MCOUNTEREN: return "mcounteren";
        case CSR_MSTATUSH:   return "mstatush";
        case CSR_MSCRATCH:   return "mscratch";
        case CSR_MEPC:       return "mepc";
        case CSR_MCAUSE:     return "mcause";
        case CSR_MTVAL:      return "mtval";
        case CSR_MIP:        return "mip";
        default:             return NULL;
    }
}

/* A plain read of a counter has its own pseudo-instruction in objdump's output:
 * `csrr rd, instret` prints as `rdinstret rd`, and likewise for cycle/time. */
static const char *csr_rd_pseudo(uint32_t addr) {
    switch (addr) {
        case CSR_CYCLE:    return "rdcycle";
        case CSR_TIME:     return "rdtime";
        case CSR_INSTRET:  return "rdinstret";
        case CSR_CYCLEH:   return "rdcycleh";
        case CSR_TIMEH:    return "rdtimeh";
        case CSR_INSTRETH: return "rdinstreth";
        default:           return NULL;
    }
}

/* Disassemble a Zicsr instruction, matching objdump's rendering: the
 * csrw/csrr/csrs/csrc (and immediate) pseudo-instructions when rd or the
 * source is x0, and the rd<counter> pseudos for plain counter reads. */
static void disasm_csr(uint32_t inst, uint32_t f3, char *buf, size_t buflen) {
    uint32_t addr = (inst >> 20) & 0xfff;
    uint32_t rdn = rd(inst), rs1n = rs1(inst);
    const char *d = reg_abi_name(rdn), *s1 = reg_abi_name(rs1n);
    const char *csr = csr_name(addr);
    char num[8];
    if (!csr) { snprintf(num, sizeof num, "0x%x", addr); csr = num; }

    switch (f3) {
    case 0x1: /* CSRRW */
        if (rdn == 0) snprintf(buf, buflen, "csrw %s,%s", csr, s1);
        else          snprintf(buf, buflen, "csrrw %s,%s,%s", d, csr, s1);
        return;
    case 0x2: /* CSRRS — a bare read (rs1 == x0) collapses to csrr / rd<counter> */
        if (rs1n == 0) {
            const char *p = csr_rd_pseudo(addr);
            if (p) snprintf(buf, buflen, "%s %s", p, d);
            else   snprintf(buf, buflen, "csrr %s,%s", d, csr);
        } else if (rdn == 0) snprintf(buf, buflen, "csrs %s,%s", csr, s1);
        else                 snprintf(buf, buflen, "csrrs %s,%s,%s", d, csr, s1);
        return;
    case 0x3: /* CSRRC */
        if (rdn == 0) snprintf(buf, buflen, "csrc %s,%s", csr, s1);
        else          snprintf(buf, buflen, "csrrc %s,%s,%s", d, csr, s1);
        return;
    case 0x5: /* CSRRWI */
        if (rdn == 0) snprintf(buf, buflen, "csrwi %s,%u", csr, rs1n);
        else          snprintf(buf, buflen, "csrrwi %s,%s,%u", d, csr, rs1n);
        return;
    case 0x6: /* CSRRSI */
        if (rdn == 0) snprintf(buf, buflen, "csrsi %s,%u", csr, rs1n);
        else          snprintf(buf, buflen, "csrrsi %s,%s,%u", d, csr, rs1n);
        return;
    case 0x7: /* CSRRCI */
        if (rdn == 0) snprintf(buf, buflen, "csrci %s,%u", csr, rs1n);
        else          snprintf(buf, buflen, "csrrci %s,%s,%u", d, csr, rs1n);
        return;
    default:
        snprintf(buf, buflen, ".word 0x%08x", inst);
        return;
    }
}

/* Disassemble an OP-FP instruction (M20). Not pinned by check-disasm (that
 * covers the RV32 suite, which has no float), so this is best-effort for
 * --trace readability rather than an exact objdump match. */
static void disasm_fp(uint32_t inst, char *buf, size_t buflen) {
    uint32_t op = inst >> 27, fmt = (inst >> 25) & 3, f3 = funct3(inst), rs2n = rs2(inst);
    const char *w = fmt ? "d" : "s";
    const char *fd = freg_abi_name(rd(inst)), *f1 = freg_abi_name(rs1(inst));
    const char *f2 = freg_abi_name(rs2(inst));
    const char *xd = reg_abi_name(rd(inst)), *x1 = reg_abi_name(rs1(inst));
    switch (op) {
    case 0x00: snprintf(buf, buflen, "fadd.%s %s,%s,%s", w, fd, f1, f2); return;
    case 0x01: snprintf(buf, buflen, "fsub.%s %s,%s,%s", w, fd, f1, f2); return;
    case 0x02: snprintf(buf, buflen, "fmul.%s %s,%s,%s", w, fd, f1, f2); return;
    case 0x03: snprintf(buf, buflen, "fdiv.%s %s,%s,%s", w, fd, f1, f2); return;
    case 0x0b: snprintf(buf, buflen, "fsqrt.%s %s,%s", w, fd, f1); return;
    case 0x04:
        if (f3 == 0 && rs1(inst) == rs2n) snprintf(buf, buflen, "fmv.%s %s,%s", w, fd, f1);
        else snprintf(buf, buflen, "%s.%s %s,%s,%s",
                      f3 == 0 ? "fsgnj" : f3 == 1 ? "fsgnjn" : "fsgnjx", w, fd, f1, f2);
        return;
    case 0x05: snprintf(buf, buflen, "%s.%s %s,%s,%s", f3 ? "fmax" : "fmin", w, fd, f1, f2); return;
    case 0x08: snprintf(buf, buflen, fmt ? "fcvt.d.s %s,%s" : "fcvt.s.d %s,%s", fd, f1); return;
    case 0x14: snprintf(buf, buflen, "%s.%s %s,%s,%s",
                        f3 == 2 ? "feq" : f3 == 1 ? "flt" : "fle", w, xd, f1, f2); return;
    case 0x1c:
        if (f3 == 0) snprintf(buf, buflen, fmt ? "fmv.x.d %s,%s" : "fmv.x.w %s,%s", xd, f1);
        else snprintf(buf, buflen, "fclass.%s %s,%s", w, xd, f1);
        return;
    case 0x1e: snprintf(buf, buflen, fmt ? "fmv.d.x %s,%s" : "fmv.w.x %s,%s", fd, x1); return;
    case 0x18: snprintf(buf, buflen, "fcvt.%s.%s %s,%s",
                        rs2n == 0 ? "w" : rs2n == 1 ? "wu" : rs2n == 2 ? "l" : "lu", w, xd, f1); return;
    case 0x1a: snprintf(buf, buflen, "fcvt.%s.%s %s,%s", w,
                        rs2n == 0 ? "w" : rs2n == 1 ? "wu" : rs2n == 2 ? "l" : "lu", fd, x1); return;
    default: snprintf(buf, buflen, ".word 0x%08x", inst); return;
    }
}

void disasm(uint64_t pc, uint32_t inst, int xlen, char *buf, size_t buflen) {
    /* A compressed (16-bit) instruction is the low halfword when its low two
     * bits are not 0b11. Expand it to the 32-bit instruction it abbreviates and
     * disassemble that — objdump prints the same expanded mnemonic. */
    if ((inst & 0x3u) != 0x3u) {
        uint32_t x = rvc_expand((uint16_t)inst, xlen == 64);
        if (x == RVC_ILLEGAL) {
            snprintf(buf, buflen, ".short 0x%04x", inst & 0xffffu);
            return;
        }
        /* C.MV expands to `add rd, x0, rs2`; objdump renders that as `mv` only
         * for the compressed form (a 32-bit add with a zero source stays `add`),
         * so handle it here before the shared 32-bit path. */
        if (opcode(x) == OP_REG && funct3(x) == 0 && funct7(x) == 0 && rs1(x) == 0) {
            snprintf(buf, buflen, "mv %s,%s",
                     reg_abi_name(rd(x)), reg_abi_name(rs2(x)));
            return;
        }
        inst = x;
    }

    uint32_t op = opcode(inst);
    uint32_t f3 = funct3(inst);
    uint32_t f7 = funct7(inst);
    const char *d  = reg_abi_name(rd(inst));
    const char *s1 = reg_abi_name(rs1(inst));
    const char *s2 = reg_abi_name(rs2(inst));

    switch (op) {
    case OP_LUI:
        snprintf(buf, buflen, "lui %s,0x%x", d, inst >> 12);
        return;

    case OP_AUIPC:
        snprintf(buf, buflen, "auipc %s,0x%x", d, inst >> 12);
        return;

    case OP_JAL: {
        uint32_t tgt = pc + (uint32_t)imm_j(inst);
        uint32_t r = rd(inst);
        if (r == 0)      snprintf(buf, buflen, "j 0x%08x", tgt);
        else if (r == 1) snprintf(buf, buflen, "jal 0x%08x", tgt);   /* ra implied */
        else             snprintf(buf, buflen, "jal %s,0x%08x", d, tgt);
        return;
    }

    case OP_JALR: {
        int32_t imm = imm_i(inst);
        uint32_t r = rd(inst), a = rs1(inst);
        if (r == 0 && imm == 0 && a == 1) snprintf(buf, buflen, "ret");
        else if (r == 0 && imm == 0)      snprintf(buf, buflen, "jr %s", s1);
        else if (r == 1 && imm == 0)      snprintf(buf, buflen, "jalr %s", s1); /* ra implied */
        else                              snprintf(buf, buflen, "jalr %s,%d(%s)", d, imm, s1);
        return;
    }

    case OP_BRANCH: {
        uint32_t tgt = pc + (uint32_t)imm_b(inst);
        int z1 = (rs1(inst) == 0), z2 = (rs2(inst) == 0);
        switch (f3) {
        case 0x0: /* BEQ  */
            if (z2) snprintf(buf, buflen, "beqz %s,0x%08x", s1, tgt);
            else    snprintf(buf, buflen, "beq %s,%s,0x%08x", s1, s2, tgt);
            return;
        case 0x1: /* BNE  */
            if (z2) snprintf(buf, buflen, "bnez %s,0x%08x", s1, tgt);
            else    snprintf(buf, buflen, "bne %s,%s,0x%08x", s1, s2, tgt);
            return;
        case 0x4: /* BLT  */
            if (z2)      snprintf(buf, buflen, "bltz %s,0x%08x", s1, tgt);
            else if (z1) snprintf(buf, buflen, "bgtz %s,0x%08x", s2, tgt);
            else         snprintf(buf, buflen, "blt %s,%s,0x%08x", s1, s2, tgt);
            return;
        case 0x5: /* BGE  */
            if (z2)      snprintf(buf, buflen, "bgez %s,0x%08x", s1, tgt);
            else if (z1) snprintf(buf, buflen, "blez %s,0x%08x", s2, tgt);
            else         snprintf(buf, buflen, "bge %s,%s,0x%08x", s1, s2, tgt);
            return;
        case 0x6: snprintf(buf, buflen, "bltu %s,%s,0x%08x", s1, s2, tgt); return;
        case 0x7: snprintf(buf, buflen, "bgeu %s,%s,0x%08x", s1, s2, tgt); return;
        default:  snprintf(buf, buflen, ".word 0x%08x", inst); return;
        }
    }

    case OP_LOAD: {
        int32_t imm = imm_i(inst);
        const char *m = (f3 == 0x0) ? "lb"  : (f3 == 0x1) ? "lh"  :
                        (f3 == 0x2) ? "lw"  : (f3 == 0x3) ? "ld"  :
                        (f3 == 0x4) ? "lbu" : (f3 == 0x5) ? "lhu" :
                        (f3 == 0x6) ? "lwu" : NULL;
        if (m) snprintf(buf, buflen, "%s %s,%d(%s)", m, d, imm, s1);
        else   snprintf(buf, buflen, ".word 0x%08x", inst);
        return;
    }

    case OP_STORE: {
        int32_t imm = imm_s(inst);
        const char *m = (f3 == 0x0) ? "sb" : (f3 == 0x1) ? "sh" :
                        (f3 == 0x2) ? "sw" : (f3 == 0x3) ? "sd" : NULL;
        if (m) snprintf(buf, buflen, "%s %s,%d(%s)", m, s2, imm, s1);
        else   snprintf(buf, buflen, ".word 0x%08x", inst);
        return;
    }

    case OP_AMO: {
        /* RV-A: funct5 picks the op, funct3 the width (2=.w, 3=.d), the aq/rl
         * bits a mnemonic suffix. LR takes no rs2 ("lr.w rd,(rs1)"); SC/AMO* are
         * "op rd,rs2,(rs1)". */
        uint32_t f5 = inst >> 27;
        const char *aqrl = (inst & (3u << 25)) == (3u << 25) ? ".aqrl"
                         : (inst & (1u << 26)) ? ".aq"
                         : (inst & (1u << 25)) ? ".rl" : "";
        const char *wd = (f3 == 0x2) ? "w" : (f3 == 0x3) ? "d" : NULL;
        if (!wd) { snprintf(buf, buflen, ".word 0x%08x", inst); return; }
        if (f5 == 0x02) { snprintf(buf, buflen, "lr.%s%s %s,(%s)", wd, aqrl, d, s1); return; }
        const char *m =
            (f5 == 0x03) ? "sc"     : (f5 == 0x01) ? "amoswap" :
            (f5 == 0x00) ? "amoadd" : (f5 == 0x04) ? "amoxor"  :
            (f5 == 0x0c) ? "amoand" : (f5 == 0x08) ? "amoor"   :
            (f5 == 0x10) ? "amomin" : (f5 == 0x14) ? "amomax"  :
            (f5 == 0x18) ? "amominu": (f5 == 0x1c) ? "amomaxu" : NULL;
        if (m) snprintf(buf, buflen, "%s.%s%s %s,%s,(%s)", m, wd, aqrl, d, s2, s1);
        else   snprintf(buf, buflen, ".word 0x%08x", inst);
        return;
    }

    case OP_FENCE:
        if (f3 == 0x1) snprintf(buf, buflen, "fence.i");
        else           snprintf(buf, buflen, "fence");
        return;

    case OP_LOAD_FP: { /* FLW / FLD */
        int32_t imm = imm_i(inst);
        const char *m = (f3 == 0x2) ? "flw" : (f3 == 0x3) ? "fld" : NULL;
        if (m) snprintf(buf, buflen, "%s %s,%d(%s)", m, freg_abi_name(rd(inst)), imm, s1);
        else   snprintf(buf, buflen, ".word 0x%08x", inst);
        return;
    }
    case OP_STORE_FP: { /* FSW / FSD */
        int32_t imm = imm_s(inst);
        const char *m = (f3 == 0x2) ? "fsw" : (f3 == 0x3) ? "fsd" : NULL;
        if (m) snprintf(buf, buflen, "%s %s,%d(%s)", m, freg_abi_name(rs2(inst)), imm, s1);
        else   snprintf(buf, buflen, ".word 0x%08x", inst);
        return;
    }
    case OP_MADD: case OP_MSUB: case OP_NMSUB: case OP_NMADD: {
        const char *m = (op == OP_MADD) ? "fmadd" : (op == OP_MSUB) ? "fmsub" :
                        (op == OP_NMSUB) ? "fnmsub" : "fnmadd";
        snprintf(buf, buflen, "%s.%s %s,%s,%s,%s", m, ((inst >> 25) & 3) ? "d" : "s",
                 freg_abi_name(rd(inst)), freg_abi_name(rs1(inst)),
                 freg_abi_name(rs2(inst)), freg_abi_name(inst >> 27));
        return;
    }
    case OP_FP:
        disasm_fp(inst, buf, buflen);
        return;

    case OP_IMM: {
        int32_t imm = imm_i(inst);
        uint32_t shamt = (inst >> 20) & (xlen == 64 ? 0x3f : 0x1f);
        switch (f3) {
        case 0x0: /* ADDI */
            if (rd(inst) == 0 && rs1(inst) == 0 && imm == 0)
                snprintf(buf, buflen, "nop");
            else if (rs1(inst) == 0)            /* li wins over mv when rs1==zero */
                snprintf(buf, buflen, "li %s,%d", d, imm);
            else if (imm == 0)
                snprintf(buf, buflen, "mv %s,%s", d, s1);
            else
                snprintf(buf, buflen, "addi %s,%s,%d", d, s1, imm);
            return;
        case 0x2: snprintf(buf, buflen, "slti %s,%s,%d", d, s1, imm); return;
        case 0x3:
            if (imm == 1) snprintf(buf, buflen, "seqz %s,%s", d, s1);
            else          snprintf(buf, buflen, "sltiu %s,%s,%d", d, s1, imm);
            return;
        case 0x4:
            if (imm == -1) snprintf(buf, buflen, "not %s,%s", d, s1);
            else           snprintf(buf, buflen, "xori %s,%s,%d", d, s1, imm);
            return;
        case 0x6: snprintf(buf, buflen, "ori %s,%s,%d", d, s1, imm); return;
        case 0x7: snprintf(buf, buflen, "andi %s,%s,%d", d, s1, imm); return;
        case 0x1: snprintf(buf, buflen, "slli %s,%s,0x%x", d, s1, shamt); return;
        case 0x5:
            if ((inst >> 30) & 1) snprintf(buf, buflen, "srai %s,%s,0x%x", d, s1, shamt);
            else                  snprintf(buf, buflen, "srli %s,%s,0x%x", d, s1, shamt);
            return;
        default: snprintf(buf, buflen, ".word 0x%08x", inst); return;
        }
    }

    case OP_IMM_32: { /* RV64 *W immediate ops */
        int32_t imm = imm_i(inst);
        uint32_t shamt = (inst >> 20) & 0x1f;
        switch (f3) {
        case 0x0:
            if (imm == 0) snprintf(buf, buflen, "sext.w %s,%s", d, s1); /* addiw rd,rs,0 */
            else          snprintf(buf, buflen, "addiw %s,%s,%d", d, s1, imm);
            return;
        case 0x1: snprintf(buf, buflen, "slliw %s,%s,0x%x", d, s1, shamt); return;
        case 0x5:
            if ((inst >> 30) & 1) snprintf(buf, buflen, "sraiw %s,%s,0x%x", d, s1, shamt);
            else                  snprintf(buf, buflen, "srliw %s,%s,0x%x", d, s1, shamt);
            return;
        default: snprintf(buf, buflen, ".word 0x%08x", inst); return;
        }
    }

    case OP_REG: {
        if (f7 == 0x01) { /* RV32M: multiply/divide share the OP opcode */
            static const char *m[8] = {
                "mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu"
            };
            snprintf(buf, buflen, "%s %s,%s,%s", m[f3], d, s1, s2);
            return;
        }
        switch (f3) {
        case 0x0:
            if (f7 == 0x20) {
                if (rs1(inst) == 0) snprintf(buf, buflen, "neg %s,%s", d, s2);
                else                snprintf(buf, buflen, "sub %s,%s,%s", d, s1, s2);
            } else                  snprintf(buf, buflen, "add %s,%s,%s", d, s1, s2);
            return;
        case 0x1: snprintf(buf, buflen, "sll %s,%s,%s", d, s1, s2); return;
        case 0x2:
            if (rs2(inst) == 0)      snprintf(buf, buflen, "sltz %s,%s", d, s1);
            else if (rs1(inst) == 0) snprintf(buf, buflen, "sgtz %s,%s", d, s2);
            else                     snprintf(buf, buflen, "slt %s,%s,%s", d, s1, s2);
            return;
        case 0x3:
            if (rs1(inst) == 0) snprintf(buf, buflen, "snez %s,%s", d, s2);
            else                snprintf(buf, buflen, "sltu %s,%s,%s", d, s1, s2);
            return;
        case 0x4: snprintf(buf, buflen, "xor %s,%s,%s", d, s1, s2); return;
        case 0x5:
            if (f7 == 0x20) snprintf(buf, buflen, "sra %s,%s,%s", d, s1, s2);
            else            snprintf(buf, buflen, "srl %s,%s,%s", d, s1, s2);
            return;
        case 0x6: snprintf(buf, buflen, "or %s,%s,%s", d, s1, s2); return;
        case 0x7: snprintf(buf, buflen, "and %s,%s,%s", d, s1, s2); return;
        default:  snprintf(buf, buflen, ".word 0x%08x", inst); return;
        }
    }

    case OP_REG_32: { /* RV64 *W register ops */
        if (f7 == 0x01) { /* RV64M *W: mulw/divw/divuw/remw/remuw */
            static const char *mw[8] = {
                "mulw", NULL, NULL, NULL, "divw", "divuw", "remw", "remuw"
            };
            if (mw[f3]) snprintf(buf, buflen, "%s %s,%s,%s", mw[f3], d, s1, s2);
            else        snprintf(buf, buflen, ".word 0x%08x", inst);
            return;
        }
        switch (f3) {
        case 0x0:
            if (f7 == 0x20) {
                if (rs1(inst) == 0) snprintf(buf, buflen, "negw %s,%s", d, s2);
                else                snprintf(buf, buflen, "subw %s,%s,%s", d, s1, s2);
            } else                  snprintf(buf, buflen, "addw %s,%s,%s", d, s1, s2);
            return;
        case 0x1: snprintf(buf, buflen, "sllw %s,%s,%s", d, s1, s2); return;
        case 0x5:
            if (f7 == 0x20) snprintf(buf, buflen, "sraw %s,%s,%s", d, s1, s2);
            else            snprintf(buf, buflen, "srlw %s,%s,%s", d, s1, s2);
            return;
        default: snprintf(buf, buflen, ".word 0x%08x", inst); return;
        }
    }

    case OP_SYSTEM: {
        uint32_t f12 = inst >> 20;
        if (f3 == 0) {
            if ((inst >> 25) == 0x09) { /* SFENCE.VMA: bare, or with vaddr/asid */
                if (rs1(inst) == 0 && rs2(inst) == 0)
                    snprintf(buf, buflen, "sfence.vma");
                else
                    snprintf(buf, buflen, "sfence.vma %s,%s", s1, s2);
            }
            else if (f12 == 0x000) snprintf(buf, buflen, "ecall");
            else if (f12 == 0x001) snprintf(buf, buflen, "ebreak");
            else if (f12 == 0x302) snprintf(buf, buflen, "mret");
            else if (f12 == 0x102) snprintf(buf, buflen, "sret");
            else if (f12 == 0x105) snprintf(buf, buflen, "wfi");
            else                   snprintf(buf, buflen, ".word 0x%08x", inst);
        } else {
            disasm_csr(inst, f3, buf, buflen);
        }
        return;
    }

    default:
        snprintf(buf, buflen, ".word 0x%08x", inst);
        return;
    }
}
