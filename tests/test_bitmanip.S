/*
 * RV32 bit-manipulation conformance (M21): Zba, Zbb, Zbs, Zbc.
 *
 * The RV32-applicable subset of the extensions (no *W / .uw forms). Plain
 * user-mode integer code, so it runs under make check (exit 0), is
 * differential-tested against qemu-riscv32 (check-diff), and its disassembly is
 * pinned against objdump (check-disasm). Built -march=rv32i_zba_zbb_zbs_zbc.
 */
#include "test_framework.h"

    TEST_START

    /* ---- Zba: shift-add ---- */
    li   t0, 5
    li   t1, 3
    sh1add a0, t0, t1
    CHECK 1, a0, 13
    sh2add a0, t0, t1
    CHECK 2, a0, 23
    sh3add a0, t0, t1
    CHECK 3, a0, 43

    /* ---- Zbb: logic-with-negate ---- */
    li   t0, 0xC
    li   t1, 0xA
    andn a0, t0, t1
    CHECK 4, a0, 4
    orn  a0, t0, t1
    CHECK 5, a0, -3
    xnor a0, t0, t1
    CHECK 6, a0, -7

    /* ---- Zbb: count and extend ---- */
    li   t0, 0xF0
    clz  a0, t0
    CHECK 7, a0, 24                     /* 32-bit leading zeros */
    ctz  a0, t0
    CHECK 8, a0, 4
    li   t0, 0xFF
    cpop a0, t0
    CHECK 9, a0, 8
    li   t0, 0x80
    sext.b a0, t0
    CHECK 10, a0, -128
    li   t0, 0x8000
    sext.h a0, t0
    CHECK 11, a0, -32768
    li   t0, 0xFFFF1234
    zext.h a0, t0
    CHECK 12, a0, 0x1234

    /* ---- Zbb: min/max ---- */
    li   t0, -5
    li   t1, 3
    min  a0, t0, t1
    CHECK 13, a0, -5
    max  a0, t0, t1
    CHECK 14, a0, 3
    minu a0, t0, t1
    CHECK 15, a0, 3
    maxu a0, t0, t1
    CHECK 16, a0, -5

    /* ---- Zbb: rotate, orc.b, rev8 ---- */
    li   t0, 1
    li   t1, 4
    rol  a0, t0, t1
    CHECK 17, a0, 16
    ror  a0, t0, t1
    CHECK 18, a0, 0x10000000            /* 1 ror 4 over 32 */
    rori a0, t0, 4
    CHECK 19, a0, 0x10000000
    li   t0, 0x010200
    orc.b a0, t0
    CHECK 20, a0, 0xFFFF00
    li   t0, 0x11223344
    rev8 a0, t0
    CHECK 21, a0, 0x44332211            /* 4-byte reversal on RV32 */

    /* ---- Zbs: single-bit register and immediate ops ---- */
    li   t0, 0
    li   t1, 3
    bset a0, t0, t1
    CHECK 22, a0, 8
    li   t0, 0xF
    li   t1, 1
    bclr a0, t0, t1
    CHECK 23, a0, 0xD
    li   t0, 0xF
    li   t1, 0
    binv a0, t0, t1
    CHECK 24, a0, 0xE
    li   t0, 0x8
    li   t1, 3
    bext a0, t0, t1
    CHECK 25, a0, 1

    li   t0, 0
    bseti a0, t0, 20
    CHECK 26, a0, 0x100000
    li   t0, 0xFF
    bclri a0, t0, 0
    CHECK 27, a0, 0xFE
    li   t0, 0
    binvi a0, t0, 10
    CHECK 28, a0, 0x400
    li   t0, 0x100
    bexti a0, t0, 8
    CHECK 29, a0, 1

    /* ---- Zbc: carry-less multiply ---- */
    li   t0, 3
    li   t1, 3
    clmul a0, t0, t1
    CHECK 30, a0, 5
    li   t0, 0x80000000
    li   t1, 2
    clmulh a0, t0, t1
    CHECK 31, a0, 1
    clmulr a0, t0, t1
    CHECK 32, a0, 2

    TEST_END
