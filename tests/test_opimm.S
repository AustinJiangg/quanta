/* RV32I conformance: OP-IMM (register/immediate arithmetic). */
#include "test_framework.h"
    TEST_START

    /* ADDI: basic, negative immediate, and 32-bit wraparound. */
    li   t0, 10
    addi t0, t0, 5
    CHECK 1, t0, 15
    li   t0, 10
    addi t0, t0, -20
    CHECK 2, t0, -10            /* 0xfffffff6 */
    li   t0, -1
    addi t0, t0, 1
    CHECK 3, t0, 0             /* wraps */

    /* SLTI: signed comparison with a sign-extended immediate. */
    li   t0, -1
    slti t1, t0, 0
    CHECK 4, t1, 1            /* -1 < 0 */
    li   t0, 5
    slti t1, t0, 5
    CHECK 5, t1, 0            /* 5 < 5 is false */

    /* SLTIU: unsigned comparison; immediate is sign-extended then unsigned. */
    li    t0, 0
    sltiu t1, t0, 1
    CHECK 6, t1, 1           /* 0 < 1 */
    li    t0, 1
    sltiu t1, t0, -1         /* imm -> 0xffffffff; 1 < 0xffffffff */
    CHECK 7, t1, 1
    li    t0, -1
    sltiu t1, t0, -1         /* 0xffffffff < 0xffffffff is false */
    CHECK 8, t1, 0

    /* XORI / ORI / ANDI, including XORI with -1 as bitwise NOT. */
    li   t0, 0x0f
    xori t1, t0, 0x3c
    CHECK 9, t1, 0x33
    li   t0, 0x0f
    ori  t1, t0, 0x30
    CHECK 10, t1, 0x3f
    li   t0, 0xff
    andi t1, t0, 0x0f
    CHECK 11, t1, 0x0f
    li   t0, 0x00ff00ff
    xori t1, t0, -1
    CHECK 12, t1, 0xff00ff00

    /* SLLI / SRLI / SRAI: logical vs arithmetic right shift. */
    li   t0, 1
    slli t1, t0, 4
    CHECK 13, t1, 0x10
    li   t0, 0x80000000
    srli t1, t0, 4
    CHECK 14, t1, 0x08000000
    li   t0, 0x80000000
    srai t1, t0, 4
    CHECK 15, t1, 0xf8000000
    li   t0, -8
    srai t1, t0, 1
    CHECK 16, t1, -4

    TEST_END
