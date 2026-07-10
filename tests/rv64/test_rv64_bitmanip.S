/*
 * RV64 bit-manipulation conformance (M21): Zba, Zbb, Zbs, Zbc.
 *
 * Each CHECK asserts an instruction's exact result, so quanta must reproduce
 * the hand-computed value (exit 0) *and* agree with qemu-riscv64 bit-for-bit —
 * the user-mode differential net in check-rv64. Covers the RV64-only forms too:
 * the *W word ops, the .uw zero-extends, and the 6-bit shift immediates.
 *
 * Built -march=rv64imac_zba_zbb_zbs_zbc (a Makefile override).
 */
#include "../test_framework.h"

    TEST_START

    /* ---- Zba: shift-add and the unsigned-word address ops ---- */
    li   t0, 5
    li   t1, 3
    sh1add a0, t0, t1
    CHECK 1, a0, 13                     /* (5<<1)+3 */
    sh2add a0, t0, t1
    CHECK 2, a0, 23                     /* (5<<2)+3 */
    sh3add a0, t0, t1
    CHECK 3, a0, 43                     /* (5<<3)+3 */

    li   t0, 0x100000005                /* low 32 bits = 5, a high bit set */
    li   t1, 3
    add.uw a0, t0, t1
    CHECK 4, a0, 8                      /* zext32(rs1)+rs2 drops the high bit */
    sh1add.uw a0, t0, t1
    CHECK 5, a0, 13
    sh2add.uw a0, t0, t1
    CHECK 6, a0, 23
    sh3add.uw a0, t0, t1
    CHECK 7, a0, 43

    li   t0, 0x100000003
    slli.uw a0, t0, 4
    CHECK 8, a0, 48                     /* zext32(3) << 4 */

    /* ---- Zbb: logic-with-negate ---- */
    li   t0, 0xC
    li   t1, 0xA
    andn a0, t0, t1
    CHECK 9, a0, 4                      /* 0xC & ~0xA */
    orn  a0, t0, t1
    CHECK 10, a0, -3                    /* 0xC | ~0xA */
    xnor a0, t0, t1
    CHECK 11, a0, -7                    /* ~(0xC ^ 0xA) */

    /* ---- Zbb: count and sign/zero-extend ---- */
    li   t0, 0xF0
    clz  a0, t0
    CHECK 12, a0, 56                    /* 64-bit leading zeros */
    ctz  a0, t0
    CHECK 13, a0, 4
    li   t0, 0xFF
    cpop a0, t0
    CHECK 14, a0, 8
    li   t0, 0x80
    sext.b a0, t0
    CHECK 15, a0, -128
    li   t0, 0x8000
    sext.h a0, t0
    CHECK 16, a0, -32768
    li   t0, 0xFFFFFFFFFFFF1234
    zext.h a0, t0
    CHECK 17, a0, 0x1234

    /* ---- Zbb: min/max, signed and unsigned ---- */
    li   t0, -5
    li   t1, 3
    min  a0, t0, t1
    CHECK 18, a0, -5
    max  a0, t0, t1
    CHECK 19, a0, 3
    minu a0, t0, t1
    CHECK 20, a0, 3                     /* -5 is a huge unsigned */
    maxu a0, t0, t1
    CHECK 21, a0, -5

    /* ---- Zbb: rotates (full-width and word) ---- */
    li   t0, 1
    li   t1, 4
    rol  a0, t0, t1
    CHECK 22, a0, 16
    ror  a0, t0, t1
    CHECK 23, a0, 0x1000000000000000    /* 1 ror 4 over 64 */
    rolw a0, t0, t1
    CHECK 24, a0, 16
    rorw a0, t0, t1
    CHECK 25, a0, 0x10000000            /* 1 ror 4 over 32, sign-extended */
    rori a0, t0, 4
    CHECK 26, a0, 0x1000000000000000
    roriw a0, t0, 4
    CHECK 27, a0, 0x10000000

    /* ---- Zbb: orc.b, rev8 ---- */
    li   t0, 0x010200
    orc.b a0, t0
    CHECK 28, a0, 0xFFFF00              /* byte set -> 0xff, byte 0 -> 0x00 */
    li   t0, 0x1122334455667788
    rev8 a0, t0
    CHECK 29, a0, 0x8877665544332211    /* 8-byte reversal on RV64 */

    /* ---- Zbb: word counts (clzw/ctzw/cpopw) ---- */
    li   t0, 0xF0
    clzw a0, t0
    CHECK 30, a0, 24                    /* 32-bit leading zeros */
    ctzw a0, t0
    CHECK 31, a0, 4
    li   t0, 0xFFFFFFFFFFFFFFFF
    cpopw a0, t0
    CHECK 32, a0, 32                    /* only the low 32 bits counted */

    /* ---- Zbs: single-bit register and immediate ops ---- */
    li   t0, 0
    li   t1, 3
    bset a0, t0, t1
    CHECK 33, a0, 8
    li   t0, 0xF
    li   t1, 1
    bclr a0, t0, t1
    CHECK 34, a0, 0xD
    li   t0, 0xF
    li   t1, 0
    binv a0, t0, t1
    CHECK 35, a0, 0xE
    li   t0, 0x8
    li   t1, 3
    bext a0, t0, t1
    CHECK 36, a0, 1

    li   t0, 0
    bseti a0, t0, 40                    /* 6-bit shift amount */
    CHECK 37, a0, 0x10000000000
    li   t0, 0xFF
    bclri a0, t0, 0
    CHECK 38, a0, 0xFE
    li   t0, 0
    binvi a0, t0, 10
    CHECK 39, a0, 0x400
    li   t0, 0x100
    bexti a0, t0, 8
    CHECK 40, a0, 1

    /* ---- Zbc: carry-less multiply ---- */
    li   t0, 3
    li   t1, 3
    clmul a0, t0, t1
    CHECK 41, a0, 5                     /* (x+1)*(x+1) = x^2+1 */
    li   t0, 0x8000000000000000
    li   t1, 2
    clmulh a0, t0, t1
    CHECK 42, a0, 1                     /* product bit 64 -> high half bit 0 */
    clmulr a0, t0, t1
    CHECK 43, a0, 2                     /* reversed: product >> 63 */

    TEST_END
