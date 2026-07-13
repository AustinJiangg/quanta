/*
 * Basic-block JIT (M25b): dynamic translation of guest RISC-V to host x86-64.
 * See jit.h for the design contract (bit-exactness, block boundaries, keying,
 * flush rules); this file is the mechanism.
 *
 * The translated code is deliberately naive — the guest register file stays
 * memory-resident in the CPU struct and every instruction loads/stores it, so
 * there is no register allocation, no condition-code tracking, and no cross-
 * instruction state to get wrong. What the JIT removes is the interpreter's
 * per-instruction overhead: fetch, length-decode, opcode dispatch, and the
 * call tree under it. Hot integer instructions become a handful of host
 * instructions; anything subtle is a call back into the same exec_* code the
 * interpreter runs (cpu_exec_alu / cpu_exec_mem), so the two engines share
 * one set of semantics.
 *
 * Generated block ABI: `uint64_t block(CPU *cpu)` (SysV: cpu in rdi). The
 * block keeps cpu in rbx (callee-saved), and returns the number of scheduler
 * steps it consumed — the caller's clocks advance by exactly that. Layout:
 *
 *     push rbx; mov rbx, rdi     ; prologue
 *     jmp  .body
 *   .epi: pop rbx; ret           ; the one exit, at a fixed offset
 *   .body: ...                   ; translated instructions
 *
 * Every exit path sets rax to its step count and jumps (backwards, to a known
 * offset) to .epi. Guest registers are addressed as [rbx + disp32]; x0 is
 * never stored to (its slot must stay zero — reg_write's invariant).
 */

/* The JIT needs pages that are both writable and executable, which ISO C
 * cannot provide — this is the project's fourth OS-specific corner (after the
 * console input and the gdbstub/NAT sockets), so the feature-test macro lives
 * here, not in the build. Translation exists only on an x86-64 POSIX host;
 * everywhere else jit_available() is 0 and the interpreter runs as always. */
#if defined(__x86_64__) && (defined(__unix__) || defined(__linux__) || \
                            (defined(__APPLE__) && defined(__MACH__)))
#define QUANTA_JIT_X86 1
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1 /* NOLINT(bugprone-reserved-identifier): feature-test macro */
#endif
#endif

#include "jit.h"
#include "cpu.h"
#include "memory.h"
#include "mmu.h"
#include "device.h"
#include "decode.h"
#include "rvc.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef QUANTA_JIT_X86
#include <sys/mman.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

/* Geometry. The block table is 4-way set-associative so two hot blocks whose
 * physical PCs collide don't retranslate each other out on every dispatch;
 * a full code arena is recycled with a whole-table flush (gen bump), which is
 * rare and self-healing. A block never exceeds JIT_MAX_INSNS instructions or
 * a page, and the per-instruction emission is bounded, so JIT_CODE_MAX is a
 * safe worst case for one block's host code. */
#define JIT_SETS      8192u
#define JIT_WAYS      4u
#define JIT_MAX_INSNS 128u
#define JIT_ARENA     (32u << 20)
#define JIT_CODE_MAX  16384u

typedef uint64_t (*JitBlockFn)(CPU *cpu);

typedef struct {
    uint64_t   gen;   /* live iff == jit->gen */
    uint64_t   pa;    /* physical address of the block's first instruction */
    uint64_t   va;    /* virtual PC it was translated for (embedded constants) */
    JitBlockFn fn;    /* entry point in the arena; NULL = untranslatable here */
    uint32_t   ninsn; /* instructions retired on the straight-line path */
} JitEntry;

struct Jit {
    uint64_t gen;     /* current generation; ++ retires every entry (flush) */
    size_t   used;    /* arena bump-allocation cursor */
    uint8_t *arena;   /* RWX code arena (mmap'd), or NULL when unavailable */
    uint32_t victim;  /* rotating eviction cursor for a full set */
    JitEntry table[JIT_SETS][JIT_WAYS];
};

int jit_available(void) {
#ifdef QUANTA_JIT_X86
    return 1;
#else
    return 0;
#endif
}

Jit *jit_new(void) {
#ifdef QUANTA_JIT_X86
    Jit *j = calloc(1, sizeof(Jit));
    if (!j) return NULL;
    j->arena = mmap(NULL, JIT_ARENA, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (j->arena == MAP_FAILED) { free(j); return NULL; }
    j->gen = 1; /* calloc'd entries (gen 0) read as stale */
    return j;
#else
    return NULL;
#endif
}

void jit_free(Jit *j) {
    if (!j) return;
#ifdef QUANTA_JIT_X86
    if (j->arena) munmap(j->arena, JIT_ARENA);
#endif
    free(j);
}

void jit_flush(Jit *j) {
    if (!j) return;
    j->gen++;    /* every table entry is now stale */
    j->used = 0; /* recycle the arena; never called from inside a block */
}

#ifdef QUANTA_JIT_X86

/* ------------------------------------------------------------------------
 * Helpers the translated code calls back into.
 *
 * Return codes shared by both (tested by the emitted glue):
 *   0 — the instruction retired; fall through to the next one.
 *   1 — it trapped or halted (raise_trap already redirected the PC); the
 *       block exits having consumed the step without retiring it.
 *   3 — aborted before execution: the access targets a device window, which
 *       must run at a true instruction boundary (an MMIO read is time-
 *       sensitive — CLINT mtime — and a store can change interrupt state).
 *       The block exits with cpu->pc already at this instruction, consuming
 *       nothing; the dispatcher's interpreter fallback re-executes it with
 *       the clocks exactly where the interpreter would have them.
 * ------------------------------------------------------------------------ */

static int jit_h_alu(CPU *cpu, uint32_t inst) {
    return cpu_exec_alu(cpu, inst);
}

static int jit_h_mem(CPU *cpu, uint32_t inst) {
    /* Probe the target physical address before executing. The probe
     * translation is idempotent (A/D updates and the TLB fill are exactly
     * what the real access would do); a probe fault is ignored here so the
     * real access below raises it through the interpreter's own path. */
    int is_store = (opcode(inst) == OP_STORE);
    uint64_t va = reg_read(cpu, rs1(inst)) +
                  (uint64_t)(int64_t)(is_store ? imm_s(inst) : imm_i(inst));
    uint64_t pa;
    uint32_t fault = mmu_translate(cpu, va, is_store ? ACC_STORE : ACC_LOAD, &pa);
    if (!fault && cpu->mem->plat && plat_contains(pa)) return 3;
    return cpu_exec_mem(cpu, inst, NULL) ? 1 : 0;
}

/* ------------------------------------------------------------------------
 * x86-64 emission. A tiny fixed-buffer assembler: each helper appends the
 * encoding of one host instruction form; `fail` latches on overflow and the
 * block is discarded. Host registers by SysV number.
 * ------------------------------------------------------------------------ */

enum { RAX = 0, RCX = 1, RBX = 3, RSI = 6, RDI = 7 };

#define ROFF(i) ((uint32_t)(offsetof(CPU, regs) + (size_t)(i) * 8))
#define PCOFF   ((uint32_t)offsetof(CPU, pc))
#define IROFF   ((uint32_t)offsetof(CPU, instret))

typedef struct {
    uint8_t  buf[JIT_CODE_MAX];
    uint32_t len;
    int      fail;
} Code;

static void eb(Code *c, uint8_t b) {
    if (c->len < JIT_CODE_MAX) c->buf[c->len++] = b;
    else c->fail = 1;
}
static void e32(Code *c, uint32_t v) {
    eb(c, (uint8_t)v); eb(c, (uint8_t)(v >> 8));
    eb(c, (uint8_t)(v >> 16)); eb(c, (uint8_t)(v >> 24));
}
static void e64(Code *c, uint64_t v) {
    e32(c, (uint32_t)v); e32(c, (uint32_t)(v >> 32));
}

/* mov r64, [rbx+off] / mov r32, [rbx+off] (zero-extends) / mov [rbx+off], r64 */
static void x_ld(Code *c, int r, uint32_t off) {
    eb(c, 0x48); eb(c, 0x8B); eb(c, (uint8_t)(0x80 | (r << 3) | RBX)); e32(c, off);
}
static void x_ld32(Code *c, int r, uint32_t off) {
    eb(c, 0x8B); eb(c, (uint8_t)(0x80 | (r << 3) | RBX)); e32(c, off);
}
static void x_st(Code *c, uint32_t off, int r) {
    eb(c, 0x48); eb(c, 0x89); eb(c, (uint8_t)(0x80 | (r << 3) | RBX)); e32(c, off);
}
/* mov r64, imm64 */
static void x_imm64(Code *c, int r, uint64_t v) {
    eb(c, 0x48); eb(c, (uint8_t)(0xB8 + r)); e64(c, v);
}
/* op dst, src (MR form; opcode 0x01 add, 0x29 sub, 0x21 and, 0x09 or,
 * 0x31 xor, 0x39 cmp), 64- and 32-bit variants */
static void x_alu(Code *c, uint8_t op, int dst, int src) {
    eb(c, 0x48); eb(c, op); eb(c, (uint8_t)(0xC0 | (src << 3) | dst));
}
static void x_alu32(Code *c, uint8_t op, int dst, int src) {
    eb(c, op); eb(c, (uint8_t)(0xC0 | (src << 3) | dst));
}
/* group-1 op r, imm32 (sign-extended in the 64-bit form); /n selects the op:
 * 0 add, 1 or, 4 and, 5 sub, 6 xor, 7 cmp */
static void x_alui(Code *c, int n, int r, uint32_t imm) {
    eb(c, 0x48); eb(c, 0x81); eb(c, (uint8_t)(0xC0 | (n << 3) | r)); e32(c, imm);
}
static void x_alui32(Code *c, int n, int r, uint32_t imm) {
    eb(c, 0x81); eb(c, (uint8_t)(0xC0 | (n << 3) | r)); e32(c, imm);
}
/* shift r by imm8 or by cl; /n: 4 shl, 5 shr, 7 sar */
static void x_shifti(Code *c, int n, int r, int w64, uint32_t sh) {
    if (w64) eb(c, 0x48);
    eb(c, 0xC1); eb(c, (uint8_t)(0xC0 | (n << 3) | r)); eb(c, (uint8_t)sh);
}
static void x_shiftcl(Code *c, int n, int r, int w64) {
    if (w64) eb(c, 0x48);
    eb(c, 0xD3); eb(c, (uint8_t)(0xC0 | (n << 3) | r));
}
/* movsxd rax, eax — re-establish the RV32 sext invariant on a 64-bit result */
static void x_sext32(Code *c) { eb(c, 0x48); eb(c, 0x63); eb(c, 0xC0); }
/* setcc al; movzx eax, al — materialise a comparison as 0/1 */
static void x_setcc(Code *c, uint8_t cc) {
    eb(c, 0x0F); eb(c, cc); eb(c, 0xC0);
    eb(c, 0x0F); eb(c, 0xB6); eb(c, 0xC0);
}
/* cmovcc dst, src (64-bit) */
static void x_cmov(Code *c, uint8_t cc, int dst, int src) {
    eb(c, 0x48); eb(c, 0x0F); eb(c, cc); eb(c, (uint8_t)(0xC0 | (dst << 3) | src));
}
/* add qword [rbx+IROFF], imm32 — credit n retired instructions */
static void x_retire(Code *c, uint32_t n) {
    if (!n) return;
    eb(c, 0x48); eb(c, 0x81); eb(c, 0x83); e32(c, IROFF); e32(c, n);
}
/* mov eax, imm32 — the block's return value (steps consumed) */
static void x_ret_steps(Code *c, uint32_t n) { eb(c, 0xB8); e32(c, n); }
/* jmp .epi (the pop/ret at fixed offset 6, always behind us) */
static void x_jmp_epi(Code *c) {
    eb(c, 0xE9); e32(c, (uint32_t)(6 - ((int32_t)c->len + 4)));
}
/* je rel8 with a backpatch handle */
static uint32_t x_je8(Code *c) {
    eb(c, 0x74); uint32_t at = c->len; eb(c, 0);
    return at;
}
static void x_patch8(Code *c, uint32_t at) {
    uint32_t dist = c->len - (at + 1);
    if (at >= JIT_CODE_MAX || dist > 127) { c->fail = 1; return; }
    c->buf[at] = (uint8_t)dist;
}

/* Call helper(cpu, inst) with cpu->pc set to this instruction first, so a
 * trap raised inside saves the correct epc (and an aborted access resumes
 * here). Clobbers rax/rcx/rdi/rsi/r10 like any SysV call; the guest state
 * lives in memory, so nothing else is live across it. */
static void x_call(Code *c, int (*fn)(CPU *, uint32_t), uint32_t inst,
                   uint64_t pc_stored) {
    x_imm64(c, RAX, pc_stored);
    x_st(c, PCOFF, RAX);
    eb(c, 0x48); eb(c, 0x89); eb(c, 0xDF);            /* mov rdi, rbx  */
    eb(c, 0xBE); e32(c, inst);                        /* mov esi, inst */
    eb(c, 0x49); eb(c, 0xBA); e64(c, (uint64_t)(uintptr_t)fn); /* mov r10, fn */
    eb(c, 0x41); eb(c, 0xFF); eb(c, 0xD2);            /* call r10      */
    eb(c, 0x85); eb(c, 0xC0);                         /* test eax, eax */
}

/* ------------------------------------------------------------------------
 * The translator: one pass over the guest instructions, emitting as it goes.
 * ------------------------------------------------------------------------ */

enum { EM_CONT, EM_TERM, EM_STOP };

/* The stored (register-file) form of an architectural value: RV32 keeps the
 * sext invariant, RV64 is the identity — the emitted constants must match
 * what reg_write/sext_xlen would have stored. */
static uint64_t sf(int rv64, uint64_t v) {
    return rv64 ? v : (uint64_t)(int64_t)(int32_t)v;
}

/* Emit the two-exit glue after an ALU helper call (status 0 or 1): fall
 * through on retire, exit on a trap. `k` is this instruction's index. */
static void glue_alu(Code *c, uint32_t k) {
    uint32_t cont = x_je8(c);
    x_retire(c, k);            /* trapped: the k earlier instructions retired */
    x_ret_steps(c, k + 1);     /* ...and the trapping step was consumed */
    x_jmp_epi(c);
    x_patch8(c, cont);
}

/* The three-exit glue after a memory helper call (status 0, 1 or 3). */
static void glue_mem(Code *c, uint32_t k) {
    uint32_t cont = x_je8(c);
    eb(c, 0x83); eb(c, 0xF8); eb(c, 0x03);  /* cmp eax, 3 */
    uint32_t abort_ = x_je8(c);
    x_retire(c, k);            /* trap: as glue_alu */
    x_ret_steps(c, k + 1);
    x_jmp_epi(c);
    x_patch8(c, abort_);       /* device access: consumed nothing; pc is */
    x_retire(c, k);            /* already at this instruction, so the    */
    x_ret_steps(c, k);         /* interpreter re-executes it next round  */
    x_jmp_epi(c);
    x_patch8(c, cont);
}

/* Write rax (a finished 64-bit result) to guest rd, re-sign-extending for
 * RV32 when the producing op was arithmetic. rd == x0 stores nothing. */
static void put_rd(Code *c, int rv64, uint32_t rdn, int needs_sext) {
    if (!rv64 && needs_sext) x_sext32(c);
    if (rdn) x_st(c, ROFF(rdn), RAX);
}

/* OP-IMM: inline the base encodings, helper the rest (bit-manip). The inline
 * conditions mirror exec_op_imm/exec_bitmanip_imm exactly: shift encodings
 * are discriminated by funct6, everything else is always base. */
static int emit_op_imm(Code *c, int rv64, uint32_t inst, uint64_t vs, uint32_t k) {
    uint32_t f3 = funct3(inst), f6 = (inst >> 26) & 0x3f, rdn = rd(inst);
    uint32_t immw = (uint32_t)imm_i(inst); /* group-1 sign-extends imm32 */
    uint32_t shmask = rv64 ? 0x3fu : 0x1fu;
    uint32_t shamt = (inst >> 20) & shmask;

    switch (f3) {
    case 0x0: /* ADDI */
        x_ld(c, RAX, ROFF(rs1(inst)));
        x_alui(c, 0, RAX, immw);
        put_rd(c, rv64, rdn, 1);
        return EM_CONT;
    case 0x2: case 0x3: /* SLTI / SLTIU */
        x_ld(c, RAX, ROFF(rs1(inst)));
        x_alui(c, 7, RAX, immw);
        x_setcc(c, f3 == 0x2 ? 0x9C : 0x92); /* setl / setb */
        put_rd(c, rv64, rdn, 0);
        return EM_CONT;
    case 0x4: case 0x6: case 0x7: /* XORI / ORI / ANDI (sext-preserving) */
        x_ld(c, RAX, ROFF(rs1(inst)));
        x_alui(c, f3 == 0x4 ? 6 : (f3 == 0x6 ? 1 : 4), RAX, immw);
        put_rd(c, rv64, rdn, 0);
        return EM_CONT;
    case 0x1: /* SLLI iff funct6 == 0, else bit-manip */
        if (f6 != 0x00) break;
        x_ld(c, RAX, ROFF(rs1(inst)));
        x_shifti(c, 4, RAX, rv64, shamt);
        put_rd(c, rv64, rdn, 1);
        return EM_CONT;
    case 0x5: /* SRLI (f6 0x00) / SRAI (f6 0x10), else bit-manip */
        if (f6 != 0x00 && f6 != 0x10) break;
        x_ld(c, RAX, ROFF(rs1(inst)));
        /* the right shifts work on the architectural width */
        x_shifti(c, f6 == 0x10 ? 7 : 5, RAX, rv64, shamt);
        put_rd(c, rv64, rdn, 1);
        return EM_CONT;
    default:
        break;
    }
    x_call(c, jit_h_alu, inst, vs);
    glue_alu(c, k);
    return EM_CONT;
}

/* OP: inline funct7 0x00 (the full base row) and 0x20's SUB/SRA; helper for
 * muldiv (0x01) and the bit-manip rows. */
static int emit_op(Code *c, int rv64, uint32_t inst, uint64_t vs, uint32_t k) {
    uint32_t f3 = funct3(inst), f7 = funct7(inst), rdn = rd(inst);
    int base = (f7 == 0x00) || (f7 == 0x20 && (f3 == 0x0 || f3 == 0x5));
    if (!base) {
        x_call(c, jit_h_alu, inst, vs);
        glue_alu(c, k);
        return EM_CONT;
    }
    x_ld(c, RAX, ROFF(rs1(inst)));
    x_ld(c, RCX, ROFF(rs2(inst)));
    switch (f3) {
    case 0x0: x_alu(c, f7 == 0x20 ? 0x29 : 0x01, RAX, RCX); /* SUB / ADD */
              put_rd(c, rv64, rdn, 1); break;
    case 0x1: x_shiftcl(c, 4, RAX, rv64);                   /* SLL (cl masks) */
              put_rd(c, rv64, rdn, 1); break;
    case 0x2: x_alu(c, 0x39, RAX, RCX); x_setcc(c, 0x9C);   /* SLT  */
              put_rd(c, rv64, rdn, 0); break;
    case 0x3: x_alu(c, 0x39, RAX, RCX); x_setcc(c, 0x92);   /* SLTU */
              put_rd(c, rv64, rdn, 0); break;
    case 0x4: x_alu(c, 0x31, RAX, RCX); put_rd(c, rv64, rdn, 0); break; /* XOR */
    case 0x5: x_shiftcl(c, f7 == 0x20 ? 7 : 5, RAX, rv64);  /* SRA / SRL */
              put_rd(c, rv64, rdn, 1); break;
    case 0x6: x_alu(c, 0x09, RAX, RCX); put_rd(c, rv64, rdn, 0); break; /* OR  */
    case 0x7: x_alu(c, 0x21, RAX, RCX); put_rd(c, rv64, rdn, 0); break; /* AND */
    }
    return EM_CONT;
}

/* OP-IMM-32 (RV64): ADDIW and the word shifts; the .uw/word-scan encodings go
 * to the helper. Results are 32-bit, sign-extended — movsxd is the invariant. */
static int emit_op_imm32(Code *c, uint32_t inst, uint64_t vs, uint32_t k) {
    uint32_t f3 = funct3(inst), f6 = (inst >> 26) & 0x3f, rdn = rd(inst);
    uint32_t shamt = (inst >> 20) & 0x1f;
    int ok = (f3 == 0x0) || (f3 == 0x1 && f6 == 0x00) ||
             (f3 == 0x5 && (f6 == 0x00 || f6 == 0x10));
    if (!ok) {
        x_call(c, jit_h_alu, inst, vs);
        glue_alu(c, k);
        return EM_CONT;
    }
    x_ld32(c, RAX, ROFF(rs1(inst)));
    if (f3 == 0x0)      x_alui32(c, 0, RAX, (uint32_t)imm_i(inst)); /* ADDIW  */
    else if (f3 == 0x1) x_shifti(c, 4, RAX, 0, shamt);              /* SLLIW  */
    else                x_shifti(c, f6 == 0x10 ? 7 : 5, RAX, 0, shamt);
    x_sext32(c);
    if (rdn) x_st(c, ROFF(rdn), RAX);
    return EM_CONT;
}

/* OP-32 (RV64): ADDW/SUBW and the word shifts; muldivw and the bit-manip
 * rows (and the encodings exec_op32 would fault on) go to the helper. */
static int emit_op32(Code *c, uint32_t inst, uint64_t vs, uint32_t k) {
    uint32_t f3 = funct3(inst), f7 = funct7(inst), rdn = rd(inst);
    int ok = (f7 == 0x00 && (f3 == 0x0 || f3 == 0x1 || f3 == 0x5)) ||
             (f7 == 0x20 && (f3 == 0x0 || f3 == 0x5));
    if (!ok) {
        x_call(c, jit_h_alu, inst, vs);
        glue_alu(c, k);
        return EM_CONT;
    }
    x_ld32(c, RAX, ROFF(rs1(inst)));
    x_ld32(c, RCX, ROFF(rs2(inst)));
    if (f3 == 0x0)      x_alu32(c, f7 == 0x20 ? 0x29 : 0x01, RAX, RCX);
    else if (f3 == 0x1) x_shiftcl(c, 4, RAX, 0);        /* SLLW (cl masks &31) */
    else                x_shiftcl(c, f7 == 0x20 ? 7 : 5, RAX, 0);
    x_sext32(c);
    if (rdn) x_st(c, ROFF(rdn), RAX);
    return EM_CONT;
}

/* One guest instruction at arch address `va` (index `k` in the block).
 * Returns EM_CONT (retires, fall through), EM_TERM (a control transfer was
 * emitted and the block is complete), or EM_STOP (not translatable — emit
 * nothing; the block ends just before it). */
static int emit_insn(Code *c, int rv64, uint32_t inst, uint64_t va,
                     uint32_t ilen, uint32_t k) {
    uint32_t op = opcode(inst), f3 = funct3(inst), rdn = rd(inst);
    uint64_t vs = sf(rv64, va);

    switch (op) {
    case OP_LUI:
        if (rdn) {
            x_imm64(c, RAX, sf(rv64, (uint64_t)(int64_t)imm_u(inst)));
            x_st(c, ROFF(rdn), RAX);
        }
        return EM_CONT;

    case OP_AUIPC:
        if (rdn) {
            x_imm64(c, RAX, sf(rv64, va + (uint64_t)(int64_t)imm_u(inst)));
            x_st(c, ROFF(rdn), RAX);
        }
        return EM_CONT;

    case OP_IMM:    return emit_op_imm(c, rv64, inst, vs, k);
    case OP_REG:    return emit_op(c, rv64, inst, vs, k);
    case OP_IMM_32: return rv64 ? emit_op_imm32(c, inst, vs, k) : EM_STOP;
    case OP_REG_32: return rv64 ? emit_op32(c, inst, vs, k)     : EM_STOP;

    case OP_LOAD:
    case OP_STORE:
        x_call(c, jit_h_mem, inst, vs);
        glue_mem(c, k);
        return EM_CONT;

    case OP_BRANCH: {
        /* cmp on the full 64-bit register values (the sext invariant makes
         * that correct for RV32 too — same rule as exec_branch), then cmov
         * between the two precomputed next-PC constants. */
        uint64_t fall  = sf(rv64, va + ilen);
        uint64_t taken = sf(rv64, va + (uint64_t)(int64_t)imm_b(inst));
        static const uint8_t cc[8] = { 0x44, 0x45, 0, 0, 0x4C, 0x4D, 0x42, 0x43 };
        if (f3 == 0x2 || f3 == 0x3) {
            /* reserved funct3: the interpreter's switch leaves taken = 0 */
            x_imm64(c, RAX, fall);
        } else {
            x_ld(c, RAX, ROFF(rs1(inst)));
            x_ld(c, RCX, ROFF(rs2(inst)));
            x_alu(c, 0x39, RAX, RCX);   /* cmp rax, rcx */
            x_imm64(c, RAX, fall);
            x_imm64(c, RCX, taken);     /* mov leaves the flags intact */
            x_cmov(c, cc[f3], RAX, RCX);
        }
        x_st(c, PCOFF, RAX);
        x_retire(c, k + 1);
        x_ret_steps(c, k + 1);
        x_jmp_epi(c);
        return EM_TERM;
    }

    case OP_JAL:
        if (rdn) {
            x_imm64(c, RAX, sf(rv64, va + ilen));
            x_st(c, ROFF(rdn), RAX);
        }
        x_imm64(c, RAX, sf(rv64, va + (uint64_t)(int64_t)imm_j(inst)));
        x_st(c, PCOFF, RAX);
        x_retire(c, k + 1);
        x_ret_steps(c, k + 1);
        x_jmp_epi(c);
        return EM_TERM;

    case OP_JALR:
        /* target from rs1 BEFORE the link write — rd may alias rs1 (the
         * far-call thunk), the same ordering rule as the interpreter. */
        x_ld(c, RAX, ROFF(rs1(inst)));
        x_alui(c, 0, RAX, (uint32_t)imm_i(inst));
        eb(c, 0x48); eb(c, 0x83); eb(c, 0xE0); eb(c, 0xFE); /* and rax, ~1 */
        if (!rv64) x_sext32(c);
        if (rdn) {
            x_imm64(c, RCX, sf(rv64, va + ilen));
            x_st(c, ROFF(rdn), RCX);
        }
        x_st(c, PCOFF, RAX);
        x_retire(c, k + 1);
        x_ret_steps(c, k + 1);
        x_jmp_epi(c);
        return EM_TERM;

    case OP_FENCE:
        /* FENCE is an architectural no-op here; FENCE.I flushes this very
         * JIT, so it must run in the interpreter (and end the block — the
         * code after it may be what the flush is for). */
        return (f3 == 0x1) ? EM_STOP : EM_CONT;

    default:
        /* SYSTEM (CSRs, ecall/ebreak, mret/sret, wfi, sfence.vma), AMO,
         * floats: all can change privilege, gates, or global state — the
         * interpreter owns them, and a block boundary is the safe place. */
        return EM_STOP;
    }
}

/* Fetch a halfword of guest code physically, refusing anything that is not
 * plain RAM (translating from a device window would memoise a non-idempotent
 * read — same rule as the decode cache). */
static int ram_fetch16(Memory *m, uint64_t pa, uint16_t *out) {
    if (m->plat && plat_contains(pa)) return 0;
    if (pa < m->base || m->size < 2 || pa - m->base > m->size - 2) return 0;
    uint64_t off = pa - m->base;
    *out = (uint16_t)(m->data[off] | ((uint16_t)m->data[off + 1] << 8));
    return 1;
}

/* Translate the block starting at cpu->pc (physical `pa`) into `e`. On any
 * failure e->fn stays NULL and the entry remembers the PC as untranslatable,
 * so the dispatcher falls back to the interpreter without retrying. */
static void translate(Jit *j, CPU *cpu, JitEntry *e, uint64_t pa) {
    static Code c; /* 16 KiB scratch; single-threaded like the whole engine */
    int rv64 = (cpu->xlen == 64);
    uint64_t va0 = rv64 ? cpu->pc : (cpu->pc & 0xffffffffu);
    uint64_t page_end = (va0 | 0xfffu) + 1;

    c.len = 0; c.fail = 0; /* the buffer itself is overwritten as it fills */
    eb(&c, 0x53);                               /* push rbx        */
    eb(&c, 0x48); eb(&c, 0x89); eb(&c, 0xFB);   /* mov rbx, rdi    */
    eb(&c, 0xEB); eb(&c, 0x02);                 /* jmp .body       */
    eb(&c, 0x5B); eb(&c, 0xC3);                 /* .epi: pop; ret  */

    uint32_t n = 0;
    uint64_t off = 0;
    int term = 0;
    while (n < JIT_MAX_INSNS) {
        uint64_t va = va0 + off;
        uint16_t lo, hi;
        uint32_t inst, ilen;
        if (va + 2 > page_end) break;
        if (!ram_fetch16(cpu->mem, pa + off, &lo)) break;
        if ((lo & 0x3u) != 0x3u) {
            inst = rvc_expand(lo, rv64);
            ilen = 2;
            if (inst == RVC_ILLEGAL) break; /* the interpreter raises it */
        } else {
            if (va + 4 > page_end) break;   /* would straddle the page */
            if (!ram_fetch16(cpu->mem, pa + off + 2, &hi)) break;
            inst = (uint32_t)lo | ((uint32_t)hi << 16);
            ilen = 4;
        }
        int r = emit_insn(&c, rv64, inst, va, ilen, n);
        if (r == EM_STOP) break;
        n++;
        off += ilen;
        if (r == EM_TERM) { term = 1; break; }
    }
    if (n == 0) return; /* nothing translatable at this PC; e->fn stays NULL */

    if (!term) { /* fall-through tail: hand the next PC to the dispatcher */
        x_imm64(&c, RAX, sf(rv64, va0 + off));
        x_st(&c, PCOFF, RAX);
        x_retire(&c, n);
        x_ret_steps(&c, n);
        x_jmp_epi(&c);
    }
    if (c.fail) return;

    if (j->used + c.len > JIT_ARENA) {
        /* Arena full: recycle everything. Reclaim this entry under the new
         * generation — every other entry just went stale. */
        jit_flush(j);
        e->gen = j->gen;
    }
    memcpy(j->arena + j->used, c.buf, c.len);
    /* An object pointer becoming a function pointer is the JIT's defining
     * moment; go through memcpy so no integer/pointer cast is involved (the
     * representation is identical on every supported host). */
    void *entry = j->arena + j->used;
    memcpy((void *)&e->fn, (const void *)&entry, sizeof e->fn);
    e->ninsn = n;
    j->used += c.len;
}

uint64_t jit_run(Jit *j, CPU *cpu, uint64_t max_steps) {
    /* The interpreter's step prologue, verbatim: clear the per-step flags and
     * give a pending interrupt (or power-off) this instruction boundary. All
     * of it is idempotent when nothing fires, so falling back to cpu_step
     * afterwards re-runs it harmlessly. */
    cpu->mem->fault = 0;
    cpu->trapped = 0;
    if (cpu_pre_step(cpu)) return 1; /* consumed the step, retired nothing */

    uint64_t pa;
    if (mmu_translate(cpu, cpu->pc, ACC_FETCH, &pa))
        return 0; /* fetch fault: the interpreter raises it */

    JitEntry *set = j->table[(pa >> 1) & (JIT_SETS - 1)];
    JitEntry *e = NULL;
    for (unsigned w = 0; w < JIT_WAYS; w++) {
        if (set[w].gen == j->gen && set[w].pa == pa && set[w].va == cpu->pc) {
            e = &set[w];
            break;
        }
    }
    if (!e) {
        for (unsigned w = 0; w < JIT_WAYS; w++)
            if (set[w].gen != j->gen) { e = &set[w]; break; }
        if (!e) e = &set[(j->victim++) & (JIT_WAYS - 1)];
        e->gen = j->gen; e->pa = pa; e->va = cpu->pc;
        e->fn = NULL;    e->ninsn = 0;
        translate(j, cpu, e, pa);
    }
    if (!e->fn) return 0;
    if ((uint64_t)e->ninsn > max_steps) return 0;
    if ((uint64_t)e->ninsn > cpu_interrupt_horizon(cpu)) return 0;
    return e->fn(cpu);
}

#else /* !QUANTA_JIT_X86: no translation on this host */

uint64_t jit_run(Jit *j, CPU *cpu, uint64_t max_steps) {
    (void)j; (void)cpu; (void)max_steps;
    return 0;
}

#endif /* QUANTA_JIT_X86 */
