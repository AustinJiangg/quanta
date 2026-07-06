#ifndef QUANTA_SOFTFLOAT_H
#define QUANTA_SOFTFLOAT_H

#include <stdint.h>

/*
 * From-scratch IEEE-754 binary32/binary64 software floating point (M20).
 *
 * The RV F and D extensions need results that are bit-identical on every host,
 * so the emulator cannot lean on the host FPU (which may round intermediates in
 * 80-bit x87 registers, contract a*b+c into a fused op, or trap differently).
 * This is a self-contained, correctly-rounded implementation honouring all five
 * RISC-V rounding modes, subnormals, signed zeros/infinities, signalling vs
 * quiet NaNs, and the five accrued-exception flags.
 *
 * Two RISC-V-specific conventions distinguish this from a generic IEEE library:
 *   - NaN results are *not* propagated with a payload: any operation that must
 *     produce a NaN yields the single canonical quiet NaN (F32_QNAN / F64_QNAN).
 *   - Float-to-integer conversions *saturate* out-of-range inputs (and NaN) to
 *     the extreme value and raise NV, rather than wrapping.
 *
 * Values are passed as raw bit patterns (f32 in a uint32_t, f64 in a uint64_t).
 * Every operation takes the rounding mode `rm` (a RISC-V frm encoding) and ORs
 * any exceptions it raises into *flags (a RISC-V fflags bitmask); the caller
 * accumulates those into fcsr.
 */

typedef uint32_t f32;
typedef uint64_t f64;

/* Rounding modes, encoded exactly as RISC-V frm (fcsr[7:5], or a funct3 field). */
enum {
    SF_RNE = 0, /* round to nearest, ties to even            */
    SF_RTZ = 1, /* round toward zero                         */
    SF_RDN = 2, /* round down   (toward -infinity)           */
    SF_RUP = 3, /* round up     (toward +infinity)           */
    SF_RMM = 4  /* round to nearest, ties to max magnitude   */
};

/* Accrued-exception flags, encoded exactly as RISC-V fflags (fcsr[4:0]). */
enum {
    SF_NX = 0x01, /* inexact          */
    SF_UF = 0x02, /* underflow        */
    SF_OF = 0x04, /* overflow         */
    SF_DZ = 0x08, /* divide by zero   */
    SF_NV = 0x10  /* invalid operation */
};

/* The canonical quiet NaNs RISC-V produces for any NaN-generating operation. */
#define F32_QNAN 0x7fc00000u
#define F64_QNAN 0x7ff8000000000000ull

/* fclass result bits (the 10-bit mask FCLASS.S/.D writes to an integer reg). */
enum {
    FCLASS_NEG_INF    = 1u << 0,
    FCLASS_NEG_NORMAL = 1u << 1,
    FCLASS_NEG_SUBNRM = 1u << 2,
    FCLASS_NEG_ZERO   = 1u << 3,
    FCLASS_POS_ZERO   = 1u << 4,
    FCLASS_POS_SUBNRM = 1u << 5,
    FCLASS_POS_NORMAL = 1u << 6,
    FCLASS_POS_INF    = 1u << 7,
    FCLASS_SNAN       = 1u << 8,
    FCLASS_QNAN       = 1u << 9
};

/* ---- binary32 ---- */
f32 f32_add (f32 a, f32 b, int rm, unsigned *flags);
f32 f32_sub (f32 a, f32 b, int rm, unsigned *flags);
f32 f32_mul (f32 a, f32 b, int rm, unsigned *flags);
f32 f32_div (f32 a, f32 b, int rm, unsigned *flags);
f32 f32_sqrt(f32 a, int rm, unsigned *flags);
/* Fused multiply-add: a*b + c with a single rounding. The four RV fused forms
 * (fmadd/fmsub/fnmadd/fnmsub) are this with the caller flipping the sign bits of
 * a and/or c, which is exact. */
f32 f32_muladd(f32 a, f32 b, f32 c, int rm, unsigned *flags);

int f32_eq(f32 a, f32 b, unsigned *flags);   /* quiet compare (NV only on sNaN) */
int f32_lt(f32 a, f32 b, unsigned *flags);   /* signalling (NV on any NaN)      */
int f32_le(f32 a, f32 b, unsigned *flags);
f32 f32_min(f32 a, f32 b, unsigned *flags);
f32 f32_max(f32 a, f32 b, unsigned *flags);
uint32_t f32_classify(f32 a);

/* ---- binary64 ---- */
f64 f64_add (f64 a, f64 b, int rm, unsigned *flags);
f64 f64_sub (f64 a, f64 b, int rm, unsigned *flags);
f64 f64_mul (f64 a, f64 b, int rm, unsigned *flags);
f64 f64_div (f64 a, f64 b, int rm, unsigned *flags);
f64 f64_sqrt(f64 a, int rm, unsigned *flags);
f64 f64_muladd(f64 a, f64 b, f64 c, int rm, unsigned *flags);

int f64_eq(f64 a, f64 b, unsigned *flags);
int f64_lt(f64 a, f64 b, unsigned *flags);
int f64_le(f64 a, f64 b, unsigned *flags);
f64 f64_min(f64 a, f64 b, unsigned *flags);
f64 f64_max(f64 a, f64 b, unsigned *flags);
uint32_t f64_classify(f64 a);

/* ---- conversions between the two float widths ---- */
f64 f32_to_f64(f32 a, unsigned *flags);
f32 f64_to_f32(f64 a, int rm, unsigned *flags);

/* ---- float -> integer (saturating, RISC-V semantics) ---- */
int32_t  f32_to_i32(f32 a, int rm, unsigned *flags);
uint32_t f32_to_u32(f32 a, int rm, unsigned *flags);
int64_t  f32_to_i64(f32 a, int rm, unsigned *flags);
uint64_t f32_to_u64(f32 a, int rm, unsigned *flags);
int32_t  f64_to_i32(f64 a, int rm, unsigned *flags);
uint32_t f64_to_u32(f64 a, int rm, unsigned *flags);
int64_t  f64_to_i64(f64 a, int rm, unsigned *flags);
uint64_t f64_to_u64(f64 a, int rm, unsigned *flags);

/* ---- integer -> float ---- */
f32 i32_to_f32(int32_t  a, int rm, unsigned *flags);
f32 u32_to_f32(uint32_t a, int rm, unsigned *flags);
f32 i64_to_f32(int64_t  a, int rm, unsigned *flags);
f32 u64_to_f32(uint64_t a, int rm, unsigned *flags);
f64 i32_to_f64(int32_t  a, int rm, unsigned *flags);
f64 u32_to_f64(uint32_t a, int rm, unsigned *flags);
f64 i64_to_f64(int64_t  a, int rm, unsigned *flags);
f64 u64_to_f64(uint64_t a, int rm, unsigned *flags);

#endif /* QUANTA_SOFTFLOAT_H */
