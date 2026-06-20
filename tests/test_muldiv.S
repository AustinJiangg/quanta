/* RV32M conformance: multiply, divide, and remainder (the M extension).
 *
 * Built with -march=rv32im (see the Makefile override) since these
 * instructions live outside the base integer set. Focus is on the
 * easy-to-miss semantics: the three high-half multiply signednesses, signed
 * vs unsigned division, and the spec's *defined* results for divide-by-zero
 * and the INT_MIN / -1 signed overflow (RV32M faults on neither). */
#include "test_framework.h"
    TEST_START

    /* --- MUL: low 32 bits of the product, sign-agnostic. --- */
    li   t0, 100
    li   t1, 23
    mul  t2, t0, t1
    CHECK 1, t2, 2300
    li   t0, 0x10000          /* 2^16 */
    li   t1, 0x10000
    mul  t2, t0, t1           /* 2^32 -> low 32 bits are 0 */
    CHECK 2, t2, 0

    /* --- MULH / MULHU / MULHSU: high 32 bits, three signednesses on the
     *     same operands. --- */
    li     t0, 0x40000000     /* 2^30 */
    li     t1, 0x40000000
    mulh   t2, t0, t1         /* 2^60 >> 32 = 0x10000000 */
    CHECK 3, t2, 0x10000000
    li     t0, -1             /* 0xffffffff */
    li     t1, -1
    mulh   t2, t0, t1         /* (-1)*(-1) = 1, high word 0 */
    CHECK 4, t2, 0
    mulhu  t2, t0, t1         /* 0xffffffff^2 high word = 0xfffffffe */
    CHECK 5, t2, 0xfffffffe
    mulhsu t2, t0, t1         /* (-1) * 0xffffffff, high word = 0xffffffff */
    CHECK 6, t2, 0xffffffff

    /* --- DIV / DIVU: truncate toward zero; signedness changes the answer. --- */
    li    t0, -20
    li    t1, 6
    div   t2, t0, t1          /* -20 / 6 = -3 (toward zero, not -4) */
    CHECK 7, t2, -3
    li    t0, 0x80000000
    li    t1, 2
    div   t2, t0, t1          /* -2^31 / 2 = -2^30 = 0xc0000000 */
    CHECK 8, t2, 0xc0000000
    divu  t2, t0, t1          /* 0x80000000 / 2 = 0x40000000 unsigned */
    CHECK 9, t2, 0x40000000

    /* --- REM / REMU: the remainder's sign follows the dividend. --- */
    li    t0, -20
    li    t1, 6
    rem   t2, t0, t1          /* -20 - (6 * -3) = -2 */
    CHECK 10, t2, -2
    li    t0, 20
    li    t1, -6
    rem   t2, t0, t1          /* 20 - (-6 * -3) = 2 */
    CHECK 11, t2, 2

    /* --- Divide by zero: defined results, no trap. --- */
    li    t0, 17
    li    t1, 0
    div   t2, t0, t1          /* signed   /0 -> -1 */
    CHECK 12, t2, -1
    divu  t2, t0, t1          /* unsigned /0 -> all ones */
    CHECK 13, t2, 0xffffffff
    rem   t2, t0, t1          /* signed   rem/0 -> dividend */
    CHECK 14, t2, 17
    remu  t2, t0, t1          /* unsigned rem/0 -> dividend */
    CHECK 15, t2, 17

    /* --- Signed overflow: INT_MIN / -1 can't be represented. --- */
    li    t0, 0x80000000
    li    t1, -1
    div   t2, t0, t1          /* quotient -> the dividend (INT_MIN) */
    CHECK 16, t2, 0x80000000
    rem   t2, t0, t1          /* remainder -> 0 */
    CHECK 17, t2, 0

    TEST_END
