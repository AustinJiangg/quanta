#include "rvc.h"

/*
 * RV32C expander — see rvc.h. Each compressed encoding is widened to the 32-bit
 * instruction it abbreviates. The fiddly part is the immediates: like the base
 * ISA, RV32C scatters immediate bits across the halfword, and differently for
 * almost every instruction, to keep the common cases short. We extract those
 * bits by hand and rebuild the standard 32-bit immediate the base decoder
 * expects, so the existing decode/execute path needs no change.
 */

/* Bit helpers on the 16-bit instruction. */
#define BIT(c, n)        (((uint32_t)(c) >> (n)) & 1u)
#define BITS(c, hi, lo)  (((uint32_t)(c) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1u))

/* Sign-extend the low `bits` of `v`. */
static uint32_t sext(uint32_t v, unsigned bits) {
    uint32_t m = 1u << (bits - 1);
    return (v ^ m) - m;
}

/* Base-ISA major opcodes the expansions target. */
enum {
    OPC_LOAD = 0x03, OPC_IMM = 0x13, OPC_STORE = 0x23, OPC_OP = 0x33,
    OPC_LUI = 0x37, OPC_BRANCH = 0x63, OPC_JALR = 0x67, OPC_JAL = 0x6f,
    OPC_SYSTEM = 0x73
};

/* 32-bit instruction-format builders (standard RV32I field/immediate layout). */
static uint32_t r_type(uint32_t f7, uint32_t rs2, uint32_t rs1,
                       uint32_t f3, uint32_t rd, uint32_t op) {
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
static uint32_t i_type(uint32_t imm, uint32_t rs1, uint32_t f3,
                       uint32_t rd, uint32_t op) {
    return ((imm & 0xfffu) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
static uint32_t s_type(uint32_t imm, uint32_t rs2, uint32_t rs1,
                       uint32_t f3, uint32_t op) {
    return (((imm >> 5) & 0x7fu) << 25) | (rs2 << 20) | (rs1 << 15) |
           (f3 << 12) | ((imm & 0x1fu) << 7) | op;
}
static uint32_t b_type(uint32_t imm, uint32_t rs2, uint32_t rs1,
                       uint32_t f3, uint32_t op) {
    return (((imm >> 12) & 1u) << 31) | (((imm >> 5) & 0x3fu) << 25) |
           (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
           (((imm >> 1) & 0xfu) << 8) | (((imm >> 11) & 1u) << 7) | op;
}
static uint32_t u_type(uint32_t imm20, uint32_t rd, uint32_t op) {
    return ((imm20 & 0xfffffu) << 12) | (rd << 7) | op;
}
static uint32_t j_type(uint32_t imm, uint32_t rd, uint32_t op) {
    return (((imm >> 20) & 1u) << 31) | (((imm >> 1) & 0x3ffu) << 21) |
           (((imm >> 11) & 1u) << 20) | (((imm >> 12) & 0xffu) << 12) |
           (rd << 7) | op;
}

/* The three-bit register fields name x8..x15 (the "popular" registers). */
static uint32_t rdp(uint16_t c)  { return BITS(c, 4, 2) + 8; } /* rd'/rs2' */
static uint32_t rs1p(uint16_t c) { return BITS(c, 9, 7) + 8; } /* rs1' */

/* Quadrant 0: stack-relative and base+offset loads/stores, and ADDI4SPN. */
static uint32_t expand_q0(uint16_t c) {
    switch (BITS(c, 15, 13)) {
    case 0: { /* C.ADDI4SPN: addi rd', x2, nzuimm */
        uint32_t imm = (BITS(c, 12, 11) << 4) | (BITS(c, 10, 7) << 6) |
                       (BIT(c, 6) << 2) | (BIT(c, 5) << 3);
        if (imm == 0) return RVC_ILLEGAL; /* the all-zero word lands here too */
        return i_type(imm, 2, 0x0, rdp(c), OPC_IMM);
    }
    case 2: { /* C.LW: lw rd', uimm(rs1') */
        uint32_t imm = (BITS(c, 12, 10) << 3) | (BIT(c, 6) << 2) | (BIT(c, 5) << 6);
        return i_type(imm, rs1p(c), 0x2, rdp(c), OPC_LOAD);
    }
    case 6: { /* C.SW: sw rs2', uimm(rs1') */
        uint32_t imm = (BITS(c, 12, 10) << 3) | (BIT(c, 6) << 2) | (BIT(c, 5) << 6);
        return s_type(imm, rdp(c), rs1p(c), 0x2, OPC_STORE);
    }
    default: /* 1/3/5/7 are F/D loads/stores, 4 is reserved */
        return RVC_ILLEGAL;
    }
}

/* Quadrant 1: immediate arithmetic, the register ALU ops, jumps and branches. */
static uint32_t expand_q1(uint16_t c) {
    uint32_t rd = BITS(c, 11, 7);
    switch (BITS(c, 15, 13)) {
    case 0: { /* C.ADDI / C.NOP: addi rd, rd, nzimm */
        uint32_t imm = sext((BIT(c, 12) << 5) | BITS(c, 6, 2), 6);
        return i_type(imm, rd, 0x0, rd, OPC_IMM);
    }
    case 1: { /* C.JAL: jal x1, imm (RV32-only encoding) */
        uint32_t imm = sext((BIT(c, 12) << 11) | (BIT(c, 11) << 4) |
                            (BITS(c, 10, 9) << 8) | (BIT(c, 8) << 10) |
                            (BIT(c, 7) << 6) | (BIT(c, 6) << 7) |
                            (BITS(c, 5, 3) << 1) | (BIT(c, 2) << 5), 12);
        return j_type(imm, 1, OPC_JAL);
    }
    case 2: { /* C.LI: addi rd, x0, imm */
        uint32_t imm = sext((BIT(c, 12) << 5) | BITS(c, 6, 2), 6);
        return i_type(imm, 0, 0x0, rd, OPC_IMM);
    }
    case 3:
        if (rd == 2) { /* C.ADDI16SP: addi x2, x2, nzimm */
            uint32_t imm = sext((BIT(c, 12) << 9) | (BITS(c, 4, 3) << 7) |
                                (BIT(c, 5) << 6) | (BIT(c, 2) << 5) |
                                (BIT(c, 6) << 4), 10);
            if (imm == 0) return RVC_ILLEGAL;
            return i_type(imm, 2, 0x0, 2, OPC_IMM);
        } else { /* C.LUI: lui rd, nzimm[17:12] (sign-extended) */
            uint32_t imm = sext((BIT(c, 12) << 5) | BITS(c, 6, 2), 6);
            if (imm == 0) return RVC_ILLEGAL;
            return u_type(imm, rd, OPC_LUI); /* imm is the 20-bit field, sign-extended */
        }
    case 4: { /* C.SRLI / C.SRAI / C.ANDI / register ALU */
        uint32_t rp = rs1p(c);
        uint32_t shamt = (BIT(c, 12) << 5) | BITS(c, 6, 2);
        switch (BITS(c, 11, 10)) {
        case 0: /* C.SRLI */
            if (BIT(c, 12)) return RVC_ILLEGAL; /* RV32: shamt[5] must be 0 */
            return i_type(shamt, rp, 0x5, rp, OPC_IMM);
        case 1: /* C.SRAI */
            if (BIT(c, 12)) return RVC_ILLEGAL;
            return i_type(0x400u | (shamt & 0x1fu), rp, 0x5, rp, OPC_IMM);
        case 2: { /* C.ANDI: andi rd', rd', imm */
            uint32_t imm = sext((BIT(c, 12) << 5) | BITS(c, 6, 2), 6);
            return i_type(imm, rp, 0x7, rp, OPC_IMM);
        }
        default: { /* C.SUB/XOR/OR/AND (c[12]=0); the *W forms (c[12]=1) are RV64 */
            uint32_t rs2 = rdp(c);
            if (BIT(c, 12)) return RVC_ILLEGAL;
            switch (BITS(c, 6, 5)) {
            case 0: return r_type(0x20, rs2, rp, 0x0, rp, OPC_OP); /* SUB */
            case 1: return r_type(0x00, rs2, rp, 0x4, rp, OPC_OP); /* XOR */
            case 2: return r_type(0x00, rs2, rp, 0x6, rp, OPC_OP); /* OR  */
            default:return r_type(0x00, rs2, rp, 0x7, rp, OPC_OP); /* AND */
            }
        }
        }
    }
    case 5: { /* C.J: jal x0, imm */
        uint32_t imm = sext((BIT(c, 12) << 11) | (BIT(c, 11) << 4) |
                            (BITS(c, 10, 9) << 8) | (BIT(c, 8) << 10) |
                            (BIT(c, 7) << 6) | (BIT(c, 6) << 7) |
                            (BITS(c, 5, 3) << 1) | (BIT(c, 2) << 5), 12);
        return j_type(imm, 0, OPC_JAL);
    }
    case 6:   /* C.BEQZ: beq rs1', x0, imm */
    case 7: { /* C.BNEZ: bne rs1', x0, imm */
        uint32_t imm = sext((BIT(c, 12) << 8) | (BITS(c, 6, 5) << 6) |
                            (BIT(c, 2) << 5) | (BITS(c, 11, 10) << 3) |
                            (BITS(c, 4, 3) << 1), 9);
        uint32_t f3 = (BITS(c, 15, 13) == 6) ? 0x0 : 0x1;
        return b_type(imm, 0, rs1p(c), f3, OPC_BRANCH);
    }
    default:
        return RVC_ILLEGAL;
    }
}

/* Quadrant 2: stack-pointer loads/stores, shifts, and the jr/mv/add family. */
static uint32_t expand_q2(uint16_t c) {
    uint32_t rd = BITS(c, 11, 7);
    uint32_t rs2 = BITS(c, 6, 2);
    switch (BITS(c, 15, 13)) {
    case 0: /* C.SLLI: slli rd, rd, shamt */
        if (BIT(c, 12)) return RVC_ILLEGAL; /* RV32: shamt[5] must be 0 */
        return i_type(rs2, rd, 0x1, rd, OPC_IMM); /* shamt = c[6:2] */
    case 2: { /* C.LWSP: lw rd, uimm(x2) */
        if (rd == 0) return RVC_ILLEGAL;
        uint32_t imm = (BIT(c, 12) << 5) | (BITS(c, 6, 4) << 2) | (BITS(c, 3, 2) << 6);
        return i_type(imm, 2, 0x2, rd, OPC_LOAD);
    }
    case 4:
        if (BIT(c, 12) == 0) {
            if (rs2 == 0) { /* C.JR: jalr x0, 0(rs1) */
                if (rd == 0) return RVC_ILLEGAL;
                return i_type(0, rd, 0x0, 0, OPC_JALR);
            }
            return r_type(0x00, rs2, 0, 0x0, rd, OPC_OP); /* C.MV: add rd, x0, rs2 */
        } else {
            if (rd == 0 && rs2 == 0)               /* C.EBREAK */
                return i_type(1, 0, 0x0, 0, OPC_SYSTEM);
            if (rs2 == 0)                           /* C.JALR: jalr x1, 0(rs1) */
                return i_type(0, rd, 0x0, 1, OPC_JALR);
            return r_type(0x00, rs2, rd, 0x0, rd, OPC_OP); /* C.ADD: add rd, rd, rs2 */
        }
    case 6: { /* C.SWSP: sw rs2, uimm(x2) */
        uint32_t imm = (BITS(c, 12, 9) << 2) | (BITS(c, 8, 7) << 6);
        return s_type(imm, rs2, 2, 0x2, OPC_STORE);
    }
    default: /* 1/3/5/7 are F/D forms */
        return RVC_ILLEGAL;
    }
}

uint32_t rvc_expand(uint16_t c) {
    switch (c & 0x3u) {
    case 0:  return expand_q0(c);
    case 1:  return expand_q1(c);
    case 2:  return expand_q2(c);
    default: return RVC_ILLEGAL; /* 0b11 is not a compressed instruction */
    }
}
