#include "softfloat.h"

/*
 * From-scratch IEEE-754 binary32/binary64 — see softfloat.h for the contract.
 *
 * Design: one generic core parameterised by a small format descriptor (Fmt),
 * so binary32 and binary64 share the arithmetic, the normalise/round pipeline,
 * and the special-case handling; only the bit widths differ. Every value is
 * unpacked into (sign, significand, exp-of-LSB) where the magnitude is
 * `sig * 2^exp2`; each op produces a possibly-wide significand plus a sticky
 * bit and hands it to round(), which normalises, applies the rounding mode, and
 * detects overflow/underflow/inexact. Wide intermediates (a 106-bit double
 * product, a divided/rooted significand) use a two-word 128-bit helper rather
 * than a compiler __int128, keeping the code portable C11 (the same reason
 * cpu.c's mulhu64 builds the high product from 32-bit partials).
 *
 * The round-and-pack step folds the significand's implicit bit and any
 * rounding carry into the exponent with a single addition (packToF-style), so
 * the "1.111.. rounds to 10.0" carry needs no special case.
 */

/* ------------------------------------------------------------------------
 * 128-bit unsigned helpers (only what the double paths need).
 * ------------------------------------------------------------------------ */
typedef struct { uint64_t hi, lo; } u128;

static inline u128 mk128(uint64_t hi, uint64_t lo) { u128 r = { hi, lo }; return r; }
static inline int is0_128(u128 a) { return (a.hi | a.lo) == 0; }

static int clz64(uint64_t x) {
    if (!x) return 64;
    int n = 0;
    if (x <= 0x00000000FFFFFFFFull) { n += 32; x <<= 32; }
    if (x <= 0x0000FFFFFFFFFFFFull) { n += 16; x <<= 16; }
    if (x <= 0x00FFFFFFFFFFFFFFull) { n += 8;  x <<= 8;  }
    if (x <= 0x0FFFFFFFFFFFFFFFull) { n += 4;  x <<= 4;  }
    if (x <= 0x3FFFFFFFFFFFFFFFull) { n += 2;  x <<= 2;  }
    if (x <= 0x7FFFFFFFFFFFFFFFull) { n += 1; }
    return n;
}
static int clz128(u128 a) { return a.hi ? clz64(a.hi) : 64 + clz64(a.lo); }
static int msb128(u128 a) { return 127 - clz128(a); } /* index of the top set bit */

static int get_bit128(u128 a, int i) {
    if (i < 0 || i > 127) return 0;
    return (int)((i >= 64 ? (a.hi >> (i - 64)) : (a.lo >> i)) & 1);
}

/* -1 / 0 / 1 for a < b / == / >. */
static int cmp128(u128 a, u128 b) {
    if (a.hi != b.hi) return a.hi < b.hi ? -1 : 1;
    if (a.lo != b.lo) return a.lo < b.lo ? -1 : 1;
    return 0;
}
static u128 add128(u128 a, u128 b) {
    u128 r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo);
    return r;
}
static u128 sub128(u128 a, u128 b) {
    u128 r;
    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - (a.lo < b.lo);
    return r;
}
/* Left shift by n in [0,127]. */
static u128 shl128(u128 a, int n) {
    if (n <= 0) return a;
    if (n >= 64) return mk128(a.lo << (n - 64), 0);
    return mk128((a.hi << n) | (a.lo >> (64 - n)), a.lo << n);
}
/* Right shift by n, returning the floor; *lost is set if any 1 bits fell off. */
static u128 shr128_jam(u128 a, int n, int *lost) {
    if (n <= 0) { *lost = 0; return a; }
    if (n >= 128) { *lost = !is0_128(a); return mk128(0, 0); }
    u128 r;
    if (n >= 64) {
        int m = n - 64;
        uint64_t lostbits = a.lo | (m ? (a.hi & (((uint64_t)1 << m) - 1)) : 0);
        r = mk128(0, m ? (a.hi >> m) : a.hi);
        *lost = lostbits != 0;
    } else {
        uint64_t lostbits = a.lo & (((uint64_t)1 << n) - 1);
        r = mk128(a.hi >> n, (a.lo >> n) | (a.hi << (64 - n)));
        *lost = lostbits != 0;
    }
    return r;
}
/* 64x64 -> 128 product from 32-bit partials (no __int128). */
static u128 mul64(uint64_t a, uint64_t b) {
    uint64_t al = (uint32_t)a, ah = a >> 32, bl = (uint32_t)b, bh = b >> 32;
    uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    uint64_t cross = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    u128 r;
    r.lo = (ll & 0xffffffffull) | (cross << 32);
    r.hi = hh + (lh >> 32) + (hl >> 32) + (cross >> 32);
    return r;
}
/* 128/64 -> 64 quotient (+ remainder), requiring num.hi < den so it fits. */
static uint64_t udiv128by64(u128 num, uint64_t den, uint64_t *rem) {
    uint64_t q = 0, r = num.hi; /* r < den by precondition */
    for (int i = 63; i >= 0; i--) {
        int carry = (int)(r >> 63);
        r = (r << 1) | ((num.lo >> i) & 1);
        if (carry || r >= den) { r -= den; q |= (uint64_t)1 << i; }
    }
    *rem = r;
    return q;
}
/* floor(sqrt(x)) for a 128-bit radicand < 2^126 (so the root fits in 63 bits);
 * *rem_nz is set if the radicand is not a perfect square. */
static uint64_t isqrt128(u128 x, int *rem_nz) {
    uint64_t root = 0;
    u128 rem = mk128(0, 0);
    for (int i = 63; i >= 0; i--) {
        rem = shl128(rem, 2);
        rem = add128(rem, mk128(0, ((uint64_t)get_bit128(x, 2 * i + 1) << 1)
                                    | (uint64_t)get_bit128(x, 2 * i)));
        /* trial = 4*root + 1: the increment when appending a 1 bit to the root,
         * (2*root+1)^2 - (2*root)^2. May exceed 64 bits, so compare in 128. */
        u128 trial = add128(shl128(mk128(0, root), 2), mk128(0, 1));
        if (cmp128(rem, trial) >= 0) { rem = sub128(rem, trial); root = (root << 1) | 1; }
        else                        { root <<= 1; }
    }
    *rem_nz = !is0_128(rem);
    return root;
}
/* Shift a 64-bit value right by dist, jamming lost bits into bit 0 (sticky). */
static uint64_t srjam64(uint64_t a, int dist) {
    if (dist == 0) return a;
    if (dist >= 64) return a != 0;
    return (a >> dist) | ((a & (((uint64_t)1 << dist) - 1)) ? 1u : 0u);
}

/* ------------------------------------------------------------------------
 * Format descriptor and unpacked representation.
 * ------------------------------------------------------------------------ */
typedef struct { int mantbits, expbits, bias, width; } Fmt;
static const Fmt F32 = { 23, 8, 127, 32 };
static const Fmt F64 = { 52, 11, 1023, 64 };

enum { CLS_ZERO, CLS_NORMAL, CLS_INF, CLS_NAN };
typedef struct {
    int  sign;   /* 0/1 */
    int  cls;    /* CLS_* (subnormals are reported as CLS_NORMAL, normalised) */
    int  snan;   /* signalling NaN */
    u128 sig;    /* significand: magnitude == sig * 2^exp2 */
    int  exp2;   /* exponent of the significand's least-significant bit */
} Up;

static uint64_t qnan_bits(const Fmt *f) { return f->width == 32 ? F32_QNAN : F64_QNAN; }
static uint64_t sign_bit(const Fmt *f)  { return (uint64_t)1 << (f->width - 1); }

static uint64_t pack_raw(const Fmt *f, int sign, uint64_t expfield, uint64_t frac) {
    return ((uint64_t)sign << (f->width - 1))
         | (expfield << f->mantbits)
         | frac;
}
static uint64_t make_inf(const Fmt *f, int sign) {
    return pack_raw(f, sign, ((uint64_t)1 << f->expbits) - 1, 0);
}
static uint64_t make_zero(const Fmt *f, int sign) { return sign ? sign_bit(f) : 0; }

static Up unpack(const Fmt *f, uint64_t bits) {
    Up u;
    uint64_t allones = ((uint64_t)1 << f->expbits) - 1;
    uint64_t fracmask = ((uint64_t)1 << f->mantbits) - 1;
    u.sign = (int)((bits >> (f->width - 1)) & 1);
    u.snan = 0;
    uint64_t exp  = (bits >> f->mantbits) & allones;
    uint64_t frac = bits & fracmask;
    if (exp == 0) {
        if (frac == 0) { u.cls = CLS_ZERO; u.sig = mk128(0, 0); u.exp2 = 0; }
        else { /* subnormal: value = frac * 2^(1-bias-mantbits) */
            u.cls = CLS_NORMAL; u.sig = mk128(0, frac);
            u.exp2 = (1 - f->bias) - f->mantbits;
        }
    } else if (exp == allones) {
        if (frac == 0) { u.cls = CLS_INF; }
        else { u.cls = CLS_NAN; u.snan = !((frac >> (f->mantbits - 1)) & 1); }
    } else {
        u.cls = CLS_NORMAL;
        u.sig = mk128(0, ((uint64_t)1 << f->mantbits) | frac);
        u.exp2 = ((int)exp - f->bias) - f->mantbits;
    }
    return u;
}

/* ------------------------------------------------------------------------
 * Round-and-pack: the single point every arithmetic result flows through.
 * ------------------------------------------------------------------------ */

/* `exp` is the biased exponent for a value normalised to [1,2), MINUS ONE — the
 * pack below re-adds the significand's implicit bit (and any rounding carry)
 * via a single addition. `sigp` holds the significand with its leading 1 at bit
 * (mantbits + R), i.e. R round bits below the fraction. */
static uint64_t roundpack(const Fmt *f, int sign, int exp, uint64_t sigp,
                          int rm, unsigned *flags) {
    int R = f->width == 32 ? 7 : 10;
    uint64_t roundMask = ((uint64_t)1 << R) - 1;
    uint64_t half      = (uint64_t)1 << (R - 1);
    uint64_t roundInc  = half;
    if (rm != SF_RNE && rm != SF_RMM)
        roundInc = (rm == (sign ? SF_RDN : SF_RUP)) ? roundMask : 0;
    uint64_t roundBits = sigp & roundMask;

    int infExp = (int)(((uint64_t)1 << f->expbits) - 1);
    uint64_t overflowBit = f->width == 32 ? 0x80000000ull : 0x8000000000000000ull;

    if ((unsigned)exp >= (unsigned)(infExp - 2)) {
        if (exp < 0) { /* subnormal: denormalise, tracking tininess for underflow */
            int isTiny = (exp < -1) || (sigp + roundInc < overflowBit);
            sigp = srjam64(sigp, -exp);
            exp = 0;
            roundBits = sigp & roundMask;
            if (isTiny && roundBits) *flags |= SF_UF;
        } else if (exp > infExp - 2 || (sigp + roundInc >= overflowBit)) {
            *flags |= SF_OF | SF_NX;
            uint64_t inf = make_inf(f, sign);
            return roundInc ? inf : inf - 1; /* inf, or the largest finite */
        }
    }

    if (roundBits) *flags |= SF_NX;
    uint64_t sig = (sigp + roundInc) >> R;
    if (rm == SF_RNE && roundBits == half) sig &= ~(uint64_t)1; /* ties to even */
    if (sig == 0) exp = 0; /* rounded away to zero */
    /* Additive pack: sig's implicit bit adds 1 to the exp field, a rounding
     * carry adds another — so both are handled without a branch. */
    return ((uint64_t)sign << (f->width - 1)) + ((uint64_t)exp << f->mantbits) + sig;
}

/* Normalise a magnitude `mant * 2^exp2` (mant != 0), plus an incoming sticky
 * bit, into roundpack's input format, then round. */
static uint64_t round_mag(const Fmt *f, int sign, u128 mant, int exp2,
                          int sticky, int rm, unsigned *flags) {
    if (is0_128(mant)) return make_zero(f, sign);
    int leadpos = f->width == 32 ? 30 : 62;
    int mb = msb128(mant);
    int expB = exp2 + mb + f->bias;          /* biased exp for value in [1,2) */

    uint64_t sigp;
    int shift = leadpos - mb;
    if (shift >= 0) {
        sigp = shl128(mant, shift).lo;       /* leadpos < 64, so it fits the low word */
        if (sticky) sigp |= 1;
    } else {
        int lost;
        sigp = shr128_jam(mant, -shift, &lost).lo;
        if (lost || sticky) sigp |= 1;
    }
    return roundpack(f, sign, expB - 1, sigp, rm, flags);
}

/* ------------------------------------------------------------------------
 * 256-bit accumulator, wide enough to sum a double's 106-bit product and a
 * 53-bit addend *exactly* whenever they overlap — the fused multiply-add can
 * cancel across the whole width, so a narrower accumulator would drop bits and
 * mis-round. (A plain add's operands are only 53 bits, but reuse the same path.)
 * ------------------------------------------------------------------------ */
typedef struct { uint64_t w[4]; } u256; /* w[0] is least significant */
static u256 u256_from128(u128 a) { u256 r = { { a.lo, a.hi, 0, 0 } }; return r; }
static int is0_256(u256 a) { return (a.w[0] | a.w[1] | a.w[2] | a.w[3]) == 0; }
static int msb256(u256 a) {
    for (int i = 3; i >= 0; i--) if (a.w[i]) return i * 64 + (63 - clz64(a.w[i]));
    return -1;
}
static int cmp256(u256 a, u256 b) {
    for (int i = 3; i >= 0; i--) if (a.w[i] != b.w[i]) return a.w[i] < b.w[i] ? -1 : 1;
    return 0;
}
static u256 add256(u256 a, u256 b) {
    u256 r; uint64_t carry = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t s = a.w[i] + carry; uint64_t c1 = s < carry;
        s += b.w[i]; uint64_t c2 = s < b.w[i];
        r.w[i] = s; carry = c1 + c2;
    }
    return r;
}
static u256 sub256(u256 a, u256 b) { /* requires a >= b */
    u256 r; uint64_t borrow = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t bi = b.w[i] + borrow; uint64_t b0 = bi < borrow;
        uint64_t d = a.w[i] - bi; uint64_t b1 = a.w[i] < bi;
        r.w[i] = d; borrow = b0 + b1;
    }
    return r;
}
static u256 shl128_256(u128 a, int n) { /* n in [0,255] */
    u256 s = u256_from128(a), r = { { 0, 0, 0, 0 } };
    if (n >= 256) return r;
    int word = n >> 6, bit = n & 63;
    for (int i = 3; i >= word; i--) {
        uint64_t v = s.w[i - word] << bit;
        if (bit && i - word - 1 >= 0) v |= s.w[i - word - 1] >> (64 - bit);
        r.w[i] = v;
    }
    return r;
}
static u256 shr256_jam(u256 a, int n, int *lost) { /* n in [0,255] */
    if (n <= 0) { *lost = 0; return a; }
    if (n >= 256) { *lost = !is0_256(a); u256 z = { { 0, 0, 0, 0 } }; return z; }
    int word = n >> 6, bit = n & 63, L = 0;
    for (int i = 0; i < word; i++) if (a.w[i]) L = 1;
    if (bit && (a.w[word] & (((uint64_t)1 << bit) - 1))) L = 1;
    *lost = L;
    u256 r = { { 0, 0, 0, 0 } };
    for (int i = 0; i + word < 4; i++) {
        uint64_t v = a.w[i + word] >> bit;
        if (bit && i + word + 1 < 4) v |= a.w[i + word + 1] << (64 - bit);
        r.w[i] = v;
    }
    return r;
}

/* Add two magnitudes `sig*2^exp` with their signs, correctly rounded. The
 * higher-leading operand (X) is placed near the top of a 256-bit window with no
 * loss; the other is aligned into the same window, jammed only if it falls
 * entirely below it (in which case it is far enough down to be pure sticky). */
static uint64_t add_core(const Fmt *f, int sX, u128 sigX, int eX,
                         int sY, u128 sigY, int eY, int rm, unsigned *flags) {
    int lX = eX + msb128(sigX), lY = eY + msb128(sigY);
    if (lY > lX) { /* make X the higher-leading operand (only lX is used below) */
        int ts = sX; sX = sY; sY = ts;
        u128 tg = sigX; sigX = sigY; sigY = tg;
        int te = eX; eX = eY; eY = te;
        lX = lY;
    }
    int wbot = lX - 253;                 /* window bottom, 253 bits below X's lead */
    u256 A = shl128_256(sigX, eX - wbot);/* eX - wbot = 253 - msb(sigX) >= 0, exact */
    u256 B; int stickyY;
    int shY = eY - wbot;
    if (shY >= 0) { B = shl128_256(sigY, shY); stickyY = 0; }
    else          { B = shr256_jam(u256_from128(sigY), -shY, &stickyY); }

    u256 M; int sign, sticky;
    if (sX == sY) { M = add256(A, B); sign = sX; sticky = stickyY; }
    else {
        int c = cmp256(A, B);
        if (c > 0) { /* |X| > |Y|: subtracting Y's floored tail borrows one, adds sticky */
            M = sub256(A, B); sign = sX;
            if (stickyY) { M = sub256(M, u256_from128(mk128(0, 1))); sticky = 1; }
            else sticky = 0;
        } else if (c < 0) { M = sub256(B, A); sign = sY; sticky = stickyY; }
        else return make_zero(f, rm == SF_RDN); /* exact cancellation */
    }
    if (is0_256(M) && !sticky) return make_zero(f, rm == SF_RDN);

    /* Reduce the exact 256-bit sum to (mant, exp2, sticky) for round_mag. */
    int mb = msb256(M);
    u128 mant; int exp2 = wbot;
    if (mb <= 127) { mant = mk128(M.w[1], M.w[0]); }
    else {
        int lost; u256 red = shr256_jam(M, mb - 127, &lost);
        mant = mk128(red.w[1], red.w[0]); exp2 = wbot + (mb - 127);
        if (lost) sticky = 1;
    }
    return round_mag(f, sign, mant, exp2, sticky, rm, flags);
}

/* ------------------------------------------------------------------------
 * The arithmetic operations, generic over the format.
 * ------------------------------------------------------------------------ */
static uint64_t fp_add(const Fmt *f, uint64_t a, uint64_t b, int sub,
                       int rm, unsigned *flags) {
    Up ua = unpack(f, a), ub = unpack(f, b);
    if (sub) ub.sign ^= 1; /* subtraction is addition with b's sign flipped */

    if (ua.cls == CLS_NAN || ub.cls == CLS_NAN) {
        if (ua.snan || ub.snan) *flags |= SF_NV;
        return qnan_bits(f);
    }
    if (ua.cls == CLS_INF || ub.cls == CLS_INF) {
        if (ua.cls == CLS_INF && ub.cls == CLS_INF) {
            if (ua.sign != ub.sign) { *flags |= SF_NV; return qnan_bits(f); }
            return make_inf(f, ua.sign);
        }
        return make_inf(f, ua.cls == CLS_INF ? ua.sign : ub.sign);
    }
    if (ua.cls == CLS_ZERO && ub.cls == CLS_ZERO) {
        if (ua.sign == ub.sign) return make_zero(f, ua.sign);
        return make_zero(f, rm == SF_RDN); /* +0 except round-down gives -0 */
    }
    /* one operand is ±0 (both-zero handled above): result is the other, with
     * the subtraction sign flip already folded into ub.sign but not b's bits. */
    if (ua.cls == CLS_ZERO) return sub ? (b ^ sign_bit(f)) : b;
    if (ub.cls == CLS_ZERO) return a;
    return add_core(f, ua.sign, ua.sig, ua.exp2, ub.sign, ub.sig, ub.exp2, rm, flags);
}

static uint64_t fp_mul(const Fmt *f, uint64_t a, uint64_t b, int rm, unsigned *flags) {
    Up ua = unpack(f, a), ub = unpack(f, b);
    int sign = ua.sign ^ ub.sign;
    if (ua.cls == CLS_NAN || ub.cls == CLS_NAN) {
        if (ua.snan || ub.snan) *flags |= SF_NV;
        return qnan_bits(f);
    }
    if (ua.cls == CLS_INF || ub.cls == CLS_INF) {
        if (ua.cls == CLS_ZERO || ub.cls == CLS_ZERO) { *flags |= SF_NV; return qnan_bits(f); }
        return make_inf(f, sign);
    }
    if (ua.cls == CLS_ZERO || ub.cls == CLS_ZERO) return make_zero(f, sign);
    u128 prod = mul64(ua.sig.lo, ub.sig.lo);
    return round_mag(f, sign, prod, ua.exp2 + ub.exp2, 0, rm, flags);
}

/* Left-align a (sub)normal significand so its top bit sits at `mantbits`,
 * adjusting exp2 so the value is unchanged. Division and square root derive the
 * result's precision from the operand bit width, so a subnormal (with a short
 * significand) must be normalised first or the quotient/root loses bits. */
static void norm_operand(Up *u, const Fmt *f) {
    if (u->cls != CLS_NORMAL || is0_128(u->sig)) return;
    int shift = f->mantbits - msb128(u->sig); /* >= 0 for an unpacked operand */
    if (shift > 0) { u->sig = shl128(u->sig, shift); u->exp2 -= shift; }
}

static uint64_t fp_div(const Fmt *f, uint64_t a, uint64_t b, int rm, unsigned *flags) {
    Up ua = unpack(f, a), ub = unpack(f, b);
    int sign = ua.sign ^ ub.sign;
    if (ua.cls == CLS_NAN || ub.cls == CLS_NAN) {
        if (ua.snan || ub.snan) *flags |= SF_NV;
        return qnan_bits(f);
    }
    if (ua.cls == CLS_INF) {
        if (ub.cls == CLS_INF) { *flags |= SF_NV; return qnan_bits(f); }
        return make_inf(f, sign);
    }
    if (ub.cls == CLS_INF) return make_zero(f, sign);
    if (ub.cls == CLS_ZERO) {
        if (ua.cls == CLS_ZERO) { *flags |= SF_NV; return qnan_bits(f); }
        *flags |= SF_DZ; return make_inf(f, sign);
    }
    if (ua.cls == CLS_ZERO) return make_zero(f, sign);
    norm_operand(&ua, f); norm_operand(&ub, f);
    int P = f->width == 32 ? 28 : 61;    /* fraction bits of the quotient */
    u128 num = shl128(ua.sig, P);        /* num.hi < sigB, so the quotient fits 64 bits */
    uint64_t rem, q = udiv128by64(num, ub.sig.lo, &rem);
    return round_mag(f, sign, mk128(0, q), ua.exp2 - ub.exp2 - P, rem != 0, rm, flags);
}

static uint64_t fp_sqrt(const Fmt *f, uint64_t a, int rm, unsigned *flags) {
    Up ua = unpack(f, a);
    if (ua.cls == CLS_NAN) { if (ua.snan) *flags |= SF_NV; return qnan_bits(f); }
    if (ua.cls == CLS_ZERO) return a;                 /* sqrt(±0) = ±0 */
    if (ua.sign) { *flags |= SF_NV; return qnan_bits(f); } /* sqrt of a negative */
    if (ua.cls == CLS_INF) return make_inf(f, 0);
    norm_operand(&ua, f);
    int e = ua.exp2;
    u128 s = ua.sig;
    if (e & 1) { s = shl128(s, 1); e -= 1; }          /* make the exponent even */
    int P = f->width == 32 ? 28 : 35;
    u128 rad = shl128(s, 2 * P);
    int rem_nz;
    uint64_t root = isqrt128(rad, &rem_nz);
    return round_mag(f, 0, mk128(0, root), e / 2 - P, rem_nz, rm, flags);
}

static uint64_t fp_muladd(const Fmt *f, uint64_t a, uint64_t b, uint64_t c,
                          int rm, unsigned *flags) {
    Up ua = unpack(f, a), ub = unpack(f, b), uc = unpack(f, c);
    int psign = ua.sign ^ ub.sign;
    int prod_invalid = (ua.cls == CLS_INF && ub.cls == CLS_ZERO)
                    || (ua.cls == CLS_ZERO && ub.cls == CLS_INF);
    if (ua.cls == CLS_NAN || ub.cls == CLS_NAN || uc.cls == CLS_NAN || prod_invalid) {
        if (ua.snan || ub.snan || uc.snan || prod_invalid) *flags |= SF_NV;
        return qnan_bits(f);
    }
    if (ua.cls == CLS_INF || ub.cls == CLS_INF) { /* product is infinite */
        if (uc.cls == CLS_INF && uc.sign != psign) { *flags |= SF_NV; return qnan_bits(f); }
        return make_inf(f, psign);
    }
    if (uc.cls == CLS_INF) return make_inf(f, uc.sign);
    if (ua.cls == CLS_ZERO || ub.cls == CLS_ZERO) { /* product is ±0 */
        if (uc.cls == CLS_ZERO)
            return psign == uc.sign ? make_zero(f, psign) : make_zero(f, rm == SF_RDN);
        return c;
    }
    u128 sigP = mul64(ua.sig.lo, ub.sig.lo);
    int eP = ua.exp2 + ub.exp2;
    if (uc.cls == CLS_ZERO) return round_mag(f, psign, sigP, eP, 0, rm, flags);
    return add_core(f, psign, sigP, eP, uc.sign, uc.sig, uc.exp2, rm, flags);
}

/* ------------------------------------------------------------------------
 * Comparisons, min/max, classify.
 * ------------------------------------------------------------------------ */
/* Numeric order of two non-NaN operands, treating -0 == +0. */
static int fcmp(const Fmt *f, uint64_t a, uint64_t b) {
    Up ua = unpack(f, a), ub = unpack(f, b);
    if (ua.cls == CLS_ZERO && ub.cls == CLS_ZERO) return 0;
    if (ua.sign != ub.sign) return ua.sign ? -1 : 1; /* negative < positive */
    /* same sign: compare magnitudes, then flip for negatives */
    int mag;
    if (ua.cls == CLS_INF || ub.cls == CLS_INF) {
        mag = (ua.cls == CLS_INF) - (ub.cls == CLS_INF);
    } else {
        int la = ua.exp2 + (ua.cls == CLS_ZERO ? -100000 : msb128(ua.sig));
        int lb = ub.exp2 + (ub.cls == CLS_ZERO ? -100000 : msb128(ub.sig));
        if (la != lb) mag = la < lb ? -1 : 1;
        else mag = cmp128(shl128(ua.sig, 127 - msb128(ua.sig)),
                          shl128(ub.sig, 127 - msb128(ub.sig)));
    }
    return ua.sign ? -mag : mag;
}

int f32_eq(f32 a, f32 b, unsigned *flags) {
    Up ua = unpack(&F32, a), ub = unpack(&F32, b);
    if (ua.cls == CLS_NAN || ub.cls == CLS_NAN) { if (ua.snan || ub.snan) *flags |= SF_NV; return 0; }
    return fcmp(&F32, a, b) == 0;
}
int f32_lt(f32 a, f32 b, unsigned *flags) {
    Up ua = unpack(&F32, a), ub = unpack(&F32, b);
    if (ua.cls == CLS_NAN || ub.cls == CLS_NAN) { *flags |= SF_NV; return 0; }
    return fcmp(&F32, a, b) < 0;
}
int f32_le(f32 a, f32 b, unsigned *flags) {
    Up ua = unpack(&F32, a), ub = unpack(&F32, b);
    if (ua.cls == CLS_NAN || ub.cls == CLS_NAN) { *flags |= SF_NV; return 0; }
    return fcmp(&F32, a, b) <= 0;
}
int f64_eq(f64 a, f64 b, unsigned *flags) {
    Up ua = unpack(&F64, a), ub = unpack(&F64, b);
    if (ua.cls == CLS_NAN || ub.cls == CLS_NAN) { if (ua.snan || ub.snan) *flags |= SF_NV; return 0; }
    return fcmp(&F64, a, b) == 0;
}
int f64_lt(f64 a, f64 b, unsigned *flags) {
    Up ua = unpack(&F64, a), ub = unpack(&F64, b);
    if (ua.cls == CLS_NAN || ub.cls == CLS_NAN) { *flags |= SF_NV; return 0; }
    return fcmp(&F64, a, b) < 0;
}
int f64_le(f64 a, f64 b, unsigned *flags) {
    Up ua = unpack(&F64, a), ub = unpack(&F64, b);
    if (ua.cls == CLS_NAN || ub.cls == CLS_NAN) { *flags |= SF_NV; return 0; }
    return fcmp(&F64, a, b) <= 0;
}

static uint64_t fp_minmax(const Fmt *f, uint64_t a, uint64_t b, int want_max, unsigned *flags) {
    Up ua = unpack(f, a), ub = unpack(f, b);
    if (ua.cls == CLS_NAN || ub.cls == CLS_NAN) {
        if (ua.snan || ub.snan) *flags |= SF_NV;
        if (ua.cls == CLS_NAN && ub.cls == CLS_NAN) return qnan_bits(f);
        return ua.cls == CLS_NAN ? b : a; /* the non-NaN operand */
    }
    int c = fcmp(f, a, b);
    if (c == 0) { /* equal magnitude: only ±0 differ — min picks -0, max picks +0 */
        int wantneg = !want_max;
        return (ua.sign == wantneg) ? a : b;
    }
    if (want_max) return c > 0 ? a : b;
    return c < 0 ? a : b;
}
f32 f32_min(f32 a, f32 b, unsigned *flags) { return (f32)fp_minmax(&F32, a, b, 0, flags); }
f32 f32_max(f32 a, f32 b, unsigned *flags) { return (f32)fp_minmax(&F32, a, b, 1, flags); }
f64 f64_min(f64 a, f64 b, unsigned *flags) { return fp_minmax(&F64, a, b, 0, flags); }
f64 f64_max(f64 a, f64 b, unsigned *flags) { return fp_minmax(&F64, a, b, 1, flags); }

static uint32_t fp_classify(const Fmt *f, uint64_t a) {
    Up u = unpack(f, a);
    switch (u.cls) {
        case CLS_INF:  return u.sign ? FCLASS_NEG_INF : FCLASS_POS_INF;
        case CLS_ZERO: return u.sign ? FCLASS_NEG_ZERO : FCLASS_POS_ZERO;
        case CLS_NAN:  return u.snan ? FCLASS_SNAN : FCLASS_QNAN;
        default: break;
    }
    /* normal vs subnormal: exp field 0 (but nonzero frac) is subnormal */
    int subnormal = ((a >> f->mantbits) & (((uint64_t)1 << f->expbits) - 1)) == 0;
    if (subnormal) return u.sign ? FCLASS_NEG_SUBNRM : FCLASS_POS_SUBNRM;
    return u.sign ? FCLASS_NEG_NORMAL : FCLASS_POS_NORMAL;
}
uint32_t f32_classify(f32 a) { return fp_classify(&F32, a); }
uint32_t f64_classify(f64 a) { return fp_classify(&F64, a); }

/* ------------------------------------------------------------------------
 * Arithmetic wrappers (typed) — sub/fused-sign handling is left to cpu.c.
 * ------------------------------------------------------------------------ */
f32 f32_add(f32 a, f32 b, int rm, unsigned *fl) { return (f32)fp_add(&F32, a, b, 0, rm, fl); }
f32 f32_sub(f32 a, f32 b, int rm, unsigned *fl) { return (f32)fp_add(&F32, a, b, 1, rm, fl); }
f32 f32_mul(f32 a, f32 b, int rm, unsigned *fl) { return (f32)fp_mul(&F32, a, b, rm, fl); }
f32 f32_div(f32 a, f32 b, int rm, unsigned *fl) { return (f32)fp_div(&F32, a, b, rm, fl); }
f32 f32_sqrt(f32 a, int rm, unsigned *fl)       { return (f32)fp_sqrt(&F32, a, rm, fl); }
f32 f32_muladd(f32 a, f32 b, f32 c, int rm, unsigned *fl) { return (f32)fp_muladd(&F32, a, b, c, rm, fl); }
f64 f64_add(f64 a, f64 b, int rm, unsigned *fl) { return fp_add(&F64, a, b, 0, rm, fl); }
f64 f64_sub(f64 a, f64 b, int rm, unsigned *fl) { return fp_add(&F64, a, b, 1, rm, fl); }
f64 f64_mul(f64 a, f64 b, int rm, unsigned *fl) { return fp_mul(&F64, a, b, rm, fl); }
f64 f64_div(f64 a, f64 b, int rm, unsigned *fl) { return fp_div(&F64, a, b, rm, fl); }
f64 f64_sqrt(f64 a, int rm, unsigned *fl)       { return fp_sqrt(&F64, a, rm, fl); }
f64 f64_muladd(f64 a, f64 b, f64 c, int rm, unsigned *fl) { return fp_muladd(&F64, a, b, c, rm, fl); }

/* ------------------------------------------------------------------------
 * Conversions.
 * ------------------------------------------------------------------------ */
f64 f32_to_f64(f32 a, unsigned *flags) {
    Up u = unpack(&F32, a);
    if (u.cls == CLS_NAN)  { if (u.snan) *flags |= SF_NV; return F64_QNAN; }
    if (u.cls == CLS_INF)  return make_inf(&F64, u.sign);
    if (u.cls == CLS_ZERO) return make_zero(&F64, u.sign);
    return round_mag(&F64, u.sign, u.sig, u.exp2, 0, SF_RNE, flags); /* exact widening */
}
f32 f64_to_f32(f64 a, int rm, unsigned *flags) {
    Up u = unpack(&F64, a);
    if (u.cls == CLS_NAN)  { if (u.snan) *flags |= SF_NV; return F32_QNAN; }
    if (u.cls == CLS_INF)  return (f32)make_inf(&F32, u.sign);
    if (u.cls == CLS_ZERO) return (f32)make_zero(&F32, u.sign);
    return (f32)round_mag(&F32, u.sign, u.sig, u.exp2, 0, rm, flags);
}

/* Round a magnitude (sig * 2^exp2, sig fits 64 bits) to an integer per rm,
 * returning the integer magnitude, an overflow flag, and inexactness. */
static uint64_t to_int_mag(uint64_t sig, int exp2, int sign, int rm,
                           int *overflow, int *inexact) {
    *overflow = 0; *inexact = 0;
    if (sig == 0) return 0;
    if (exp2 >= 0) {
        int top = 63 - clz64(sig);
        if (top + exp2 > 63) { *overflow = 1; return 0; }
        return sig << exp2;
    }
    int rsh = -exp2;
    uint64_t floorv = rsh >= 64 ? 0 : sig >> rsh;
    int roundbit = (rsh >= 1 && rsh <= 64) ? (int)((sig >> (rsh - 1)) & 1) : 0;
    uint64_t belowmask = (rsh - 1) >= 64 ? ~(uint64_t)0
                        : (rsh >= 1 ? (((uint64_t)1 << (rsh - 1)) - 1) : 0);
    int sticky = (sig & belowmask) != 0;
    *inexact = roundbit | sticky;
    int inc = 0;
    switch (rm) {
        case SF_RNE: inc = roundbit && (sticky || (floorv & 1)); break;
        case SF_RTZ: inc = 0; break;
        case SF_RDN: inc = sign && *inexact; break;   /* toward -inf grows |neg| */
        case SF_RUP: inc = !sign && *inexact; break;  /* toward +inf grows |pos| */
        case SF_RMM: inc = roundbit; break;
    }
    return floorv + (uint64_t)inc; /* may carry into the next bit; range-checked by caller */
}

static uint64_t fp_to_int(const Fmt *f, uint64_t a, int is_signed, int intbits,
                          int rm, unsigned *flags) {
    Up u = unpack(f, a);
    uint64_t maxpos = is_signed ? (((uint64_t)1 << (intbits - 1)) - 1)
                    : (intbits >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << intbits) - 1));
    uint64_t minneg = is_signed ? ((uint64_t)1 << (intbits - 1)) : 0; /* |INT_MIN| */

    if (u.cls == CLS_NAN)  { *flags |= SF_NV; return maxpos; }
    if (u.cls == CLS_ZERO) return 0;
    if (u.cls == CLS_INF)  { *flags |= SF_NV; return u.sign ? minneg : maxpos; }

    int overflow, inexact;
    uint64_t mag = to_int_mag(u.sig.lo, u.exp2, u.sign, rm, &overflow, &inexact);
    /* a carry out of the top can push mag from maxpos to maxpos+1 */
    if (u.sign) {
        if (overflow || mag > minneg) { *flags |= SF_NV; return minneg; }
        if (!is_signed && mag != 0)   { *flags |= SF_NV; return 0; }
        if (inexact) *flags |= SF_NX;
        return (uint64_t)(-(int64_t)mag) & (intbits >= 64 ? ~(uint64_t)0
                                                          : (((uint64_t)1 << intbits) - 1));
    } else {
        if (overflow || mag > maxpos) { *flags |= SF_NV; return maxpos; }
        if (inexact) *flags |= SF_NX;
        return mag;
    }
}

int32_t  f32_to_i32(f32 a, int rm, unsigned *fl) { return (int32_t) fp_to_int(&F32, a, 1, 32, rm, fl); }
uint32_t f32_to_u32(f32 a, int rm, unsigned *fl) { return (uint32_t)fp_to_int(&F32, a, 0, 32, rm, fl); }
int64_t  f32_to_i64(f32 a, int rm, unsigned *fl) { return (int64_t) fp_to_int(&F32, a, 1, 64, rm, fl); }
uint64_t f32_to_u64(f32 a, int rm, unsigned *fl) { return          fp_to_int(&F32, a, 0, 64, rm, fl); }
int32_t  f64_to_i32(f64 a, int rm, unsigned *fl) { return (int32_t) fp_to_int(&F64, a, 1, 32, rm, fl); }
uint32_t f64_to_u32(f64 a, int rm, unsigned *fl) { return (uint32_t)fp_to_int(&F64, a, 0, 32, rm, fl); }
int64_t  f64_to_i64(f64 a, int rm, unsigned *fl) { return (int64_t) fp_to_int(&F64, a, 1, 64, rm, fl); }
uint64_t f64_to_u64(f64 a, int rm, unsigned *fl) { return          fp_to_int(&F64, a, 0, 64, rm, fl); }

static uint64_t int_to_fp(const Fmt *f, uint64_t mag, int sign, int rm, unsigned *flags) {
    if (mag == 0) return 0;
    return round_mag(f, sign, mk128(0, mag), 0, 0, rm, flags);
}
f32 i32_to_f32(int32_t a, int rm, unsigned *fl) {
    int s = a < 0; return (f32)int_to_fp(&F32, s ? -(uint64_t)a : (uint64_t)a, s, rm, fl);
}
f32 u32_to_f32(uint32_t a, int rm, unsigned *fl) { return (f32)int_to_fp(&F32, a, 0, rm, fl); }
f32 i64_to_f32(int64_t a, int rm, unsigned *fl) {
    int s = a < 0; return (f32)int_to_fp(&F32, s ? -(uint64_t)a : (uint64_t)a, s, rm, fl);
}
f32 u64_to_f32(uint64_t a, int rm, unsigned *fl) { return (f32)int_to_fp(&F32, a, 0, rm, fl); }
f64 i32_to_f64(int32_t a, int rm, unsigned *fl) {
    int s = a < 0; return int_to_fp(&F64, s ? -(uint64_t)a : (uint64_t)a, s, rm, fl);
}
f64 u32_to_f64(uint32_t a, int rm, unsigned *fl) { return int_to_fp(&F64, a, 0, rm, fl); }
f64 i64_to_f64(int64_t a, int rm, unsigned *fl) {
    int s = a < 0; return int_to_fp(&F64, s ? -(uint64_t)a : (uint64_t)a, s, rm, fl);
}
f64 u64_to_f64(uint64_t a, int rm, unsigned *fl) { return int_to_fp(&F64, a, 0, rm, fl); }
