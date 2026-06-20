/* RV32I conformance: OP (register/register arithmetic). */
#include "test_framework.h"
    TEST_START

    /* ADD / SUB, including wraparound. */
    li  t0, 100
    li  t1, 23
    add t2, t0, t1
    CHECK 1, t2, 123
    sub t2, t0, t1
    CHECK 2, t2, 77
    li  t0, 0
    li  t1, 1
    sub t2, t0, t1
    CHECK 3, t2, -1

    /* SLT (signed) vs SLTU (unsigned) on the same operands. */
    li   t0, -1
    li   t1, 1
    slt  t2, t0, t1
    CHECK 4, t2, 1            /* -1 < 1 signed */
    sltu t2, t0, t1
    CHECK 5, t2, 0            /* 0xffffffff < 1 unsigned is false */

    /* XOR / OR / AND. */
    li  t0, 0xf0f0f0f0
    li  t1, 0x0f0f0f0f
    xor t2, t0, t1
    CHECK 6, t2, 0xffffffff
    or  t2, t0, t1
    CHECK 7, t2, 0xffffffff
    and t2, t0, t1
    CHECK 8, t2, 0

    /* SLL / SRL / SRA. */
    li  t0, 1
    li  t1, 8
    sll t2, t0, t1
    CHECK 9, t2, 0x100
    li  t0, 0x80000000
    li  t1, 4
    srl t2, t0, t1
    CHECK 10, t2, 0x08000000
    sra t2, t0, t1
    CHECK 11, t2, 0xf8000000

    /* Shift amount is masked to the low 5 bits of rs2. */
    li  t0, 1
    li  t1, 33               /* 33 & 31 = 1 */
    sll t2, t0, t1
    CHECK 12, t2, 2
    li  t0, 0x80000000
    li  t1, 36               /* 36 & 31 = 4 */
    srl t2, t0, t1
    CHECK 13, t2, 0x08000000

    TEST_END
