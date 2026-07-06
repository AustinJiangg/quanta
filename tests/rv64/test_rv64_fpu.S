/*
 * RV64 F/D floating-point conformance (M20).
 *
 * Plain user-mode code — fcsr/frm/fflags are unprivileged CSRs — so it is
 * differentially tested against qemu-riscv64 (make check-rv64), the golden
 * model for IEEE-754 edge cases: correct rounding, the canonical NaN RISC-V
 * produces, saturating float->int conversions, min/max and compare NaN rules,
 * and the accrued exception flags. Each CHECK exits with its id on the first
 * mismatch; all-pass exits 0. Framework scratch: gp (x3), t6 (x31).
 */
    .include "test_framework.h"

    /* Load a 32-bit single-precision constant into \fr via integer \ir. */
    .macro LDS fr, ir, val
    li   \ir, \val
    fmv.w.x \fr, \ir
    .endm
    /* Load a 64-bit double-precision constant into \fr via integer \ir. */
    .macro LDD fr, ir, val
    li   \ir, \val
    fmv.d.x \fr, \ir
    .endm
    /* CHECK a single-precision bit pattern in \reg (zero-extended to 32 bits). */
    .macro CHECKS id, reg, val
    slli \reg, \reg, 32
    srli \reg, \reg, 32
    CHECK \id, \reg, \val
    .endm

    TEST_START

    /* ---- single-precision arithmetic ---- */
    LDS  fa0, t0, 0x3f800000        /* 1.0f */
    LDS  fa1, t0, 0x40000000        /* 2.0f */
    fadd.s ft0, fa0, fa1            /* 3.0f */
    fmv.x.w t0, ft0
    CHECKS 1, t0, 0x40400000
    fsub.s ft0, fa1, fa0            /* 1.0f */
    fmv.x.w t0, ft0
    CHECKS 2, t0, 0x3f800000
    fmul.s ft0, fa1, ft0           /* 2.0 * 1.0 = 2.0 */
    fmv.x.w t0, ft0
    CHECKS 3, t0, 0x40000000
    fdiv.s ft0, fa0, fa1           /* 0.5f */
    fmv.x.w t0, ft0
    CHECKS 4, t0, 0x3f000000
    LDS  fa2, t0, 0x40800000       /* 4.0f */
    fsqrt.s ft0, fa2               /* 2.0f */
    fmv.x.w t0, ft0
    CHECKS 5, t0, 0x40000000

    /* ---- fused multiply-add (single rounding) ---- */
    LDS  fa3, t0, 0x40400000       /* 3.0f */
    fmadd.s  ft0, fa1, fa3, fa0    /* 2*3 + 1 = 7.0 */
    fmv.x.w t0, ft0
    CHECKS 6, t0, 0x40e00000
    fmsub.s  ft0, fa1, fa3, fa0    /* 2*3 - 1 = 5.0 */
    fmv.x.w t0, ft0
    CHECKS 7, t0, 0x40a00000
    fnmsub.s ft0, fa1, fa3, fa0    /* -(2*3) + 1 = -5.0 */
    fmv.x.w t0, ft0
    CHECKS 8, t0, 0xc0a00000
    fnmadd.s ft0, fa1, fa3, fa0    /* -(2*3) - 1 = -7.0 */
    fmv.x.w t0, ft0
    CHECKS 9, t0, 0xc0e00000

    /* ---- sign injection ---- */
    LDS  fa4, t0, 0xc0000000       /* -2.0f */
    fsgnj.s  ft0, fa0, fa4         /* |1.0| with sign of -2 = -1.0 */
    fmv.x.w t0, ft0
    CHECKS 10, t0, 0xbf800000
    fsgnjn.s ft0, fa0, fa4         /* sign = ~sign(-2) = + -> 1.0 */
    fmv.x.w t0, ft0
    CHECKS 11, t0, 0x3f800000
    fsgnjx.s ft0, fa4, fa4         /* sign = sign^sign = + -> 2.0 */
    fmv.x.w t0, ft0
    CHECKS 12, t0, 0x40000000

    /* ---- min/max, including signed zero and NaN ---- */
    fmin.s ft0, fa0, fa1           /* min(1,2) = 1 */
    fmv.x.w t0, ft0
    CHECKS 13, t0, 0x3f800000
    fmax.s ft0, fa0, fa1           /* max(1,2) = 2 */
    fmv.x.w t0, ft0
    CHECKS 14, t0, 0x40000000
    LDS  ft1, t0, 0x00000000       /* +0.0 */
    LDS  ft2, t0, 0x80000000       /* -0.0 */
    fmin.s ft0, ft1, ft2           /* min(+0,-0) = -0 */
    fmv.x.w t0, ft0
    CHECKS 15, t0, 0x80000000
    fmax.s ft0, ft1, ft2           /* max(+0,-0) = +0 */
    fmv.x.w t0, ft0
    CHECKS 16, t0, 0x00000000
    LDS  ft3, t0, 0x7fc00000       /* qNaN */
    fmin.s ft0, fa0, ft3           /* min(1, NaN) = 1 (the non-NaN) */
    fmv.x.w t0, ft0
    CHECKS 17, t0, 0x3f800000

    /* ---- compares (NaN -> 0; FLT/FLE signalling) ---- */
    feq.s t0, fa0, fa0
    CHECK 18, t0, 1
    flt.s t0, fa0, fa1
    CHECK 19, t0, 1
    fle.s t0, fa1, fa0
    CHECK 20, t0, 0
    feq.s t0, fa0, ft3             /* 1 == NaN -> 0 */
    CHECK 21, t0, 0
    flt.s t0, ft3, fa0             /* NaN < 1 -> 0 */
    CHECK 22, t0, 0

    /* ---- classify ---- */
    fclass.s t0, fa0               /* +normal -> bit 6 */
    CHECK 23, t0, 0x40
    fclass.s t0, ft2               /* -0.0 -> bit 3 */
    CHECK 24, t0, 0x08
    fclass.s t0, ft3               /* qNaN -> bit 9 */
    CHECK 25, t0, 0x200
    LDS  ft4, t0, 0xff800000       /* -inf */
    fclass.s t0, ft4               /* -inf -> bit 0 */
    CHECK 26, t0, 0x01

    /* ---- float <-> integer conversions ---- */
    LDS  ft5, t0, 0x40200000       /* 2.5f */
    fcvt.w.s t0, ft5               /* RNE: 2.5 -> 2 (ties to even) */
    CHECK 27, t0, 2
    fcvt.w.s t0, ft5, rtz          /* RTZ: 2.5 -> 2 */
    CHECK 28, t0, 2
    LDS  ft5, t0, 0x40490fdb       /* pi ~ 3.1415927 */
    fcvt.w.s t0, ft5, rup          /* round up -> 4 */
    CHECK 29, t0, 4
    li   t1, 7
    fcvt.s.w ft0, t1               /* 7 -> 7.0f */
    fmv.x.w t0, ft0
    CHECKS 30, t0, 0x40e00000
    LDS  ft5, t0, 0xbf800000       /* -1.0f */
    fcvt.wu.s t0, ft5              /* negative -> unsigned: 0, NV */
    CHECK 31, t0, 0
    fcvt.w.s t0, ft3               /* NaN -> max signed int, NV */
    CHECK 32, t0, 0x7fffffff
    LDS  ft5, t0, 0x7f000000       /* ~1.7e38, overflows int32 */
    fcvt.w.s t0, ft5               /* +overflow -> INT_MAX, NV */
    CHECK 33, t0, 0x7fffffff

    /* ---- double precision ---- */
    LDD  fa0, t0, 0x3ff0000000000000   /* 1.0 */
    LDD  fa1, t0, 0x4000000000000000   /* 2.0 */
    fadd.d ft0, fa0, fa1               /* 3.0 */
    fmv.x.d t0, ft0
    CHECK 34, t0, 0x4008000000000000
    fdiv.d ft0, fa0, fa1               /* 0.5 */
    fmv.x.d t0, ft0
    CHECK 35, t0, 0x3fe0000000000000
    LDD  fa2, t0, 0x4010000000000000   /* 4.0 */
    fsqrt.d ft0, fa2                   /* 2.0 */
    fmv.x.d t0, ft0
    CHECK 36, t0, 0x4000000000000000
    fcvt.s.d ft0, fa2                  /* 4.0 (double) -> 4.0f */
    fmv.x.w t0, ft0
    CHECKS 37, t0, 0x40800000
    LDS  ft1, t0, 0x40490fdb           /* pi (single) */
    fcvt.d.s ft0, ft1                  /* widen -> double pi (exact) */
    fmv.x.d t0, ft0
    CHECK 38, t0, 0x400921fb60000000
    /* fcvt.l.d of 4.0 -> 4 (RV64 64-bit int) */
    fcvt.l.d t0, fa2
    CHECK 39, t0, 4
    li   t1, 1000000007
    fcvt.d.l ft0, t1                   /* exact for a 30-bit int */
    fmv.x.d t0, ft0
    CHECK 40, t0, 0x41cdcd6503800000

    /* ---- accrued exception flags ---- */
    csrw fflags, zero
    LDS  ft1, t0, 0x3f800000
    LDS  ft2, t0, 0x00000000
    fdiv.s ft0, ft1, ft2               /* 1.0/0.0 -> +inf, DZ */
    csrr t0, fflags
    CHECK 41, t0, 0x08
    csrw fflags, zero
    LDS  ft1, t0, 0x7f800000           /* +inf */
    LDS  ft2, t0, 0xff800000           /* -inf */
    fadd.s ft0, ft1, ft2               /* inf + -inf -> NV */
    csrr t0, fflags
    CHECK 42, t0, 0x10
    csrw fflags, zero
    LDS  ft1, t0, 0x3f800000
    LDS  ft2, t0, 0x40400000
    fdiv.s ft0, ft1, ft2               /* 1.0/3.0 -> inexact, NX */
    csrr t0, fflags
    CHECK 43, t0, 0x01

    /* ---- dynamic rounding mode via frm ---- */
    li   t0, 1                          /* frm = RTZ */
    fsrm t0
    LDS  ft1, t0, 0x3f800000            /* 1.0 */
    LDS  ft2, t0, 0x40400000            /* 3.0 */
    fdiv.s ft0, ft1, ft2               /* 1/3 RTZ -> 0x3eaaaaaa */
    fmv.x.w t0, ft0
    CHECKS 44, t0, 0x3eaaaaaa

    TEST_END
