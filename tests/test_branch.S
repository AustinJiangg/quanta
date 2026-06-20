/* RV32I conformance: BRANCH (taken/not-taken, signed vs unsigned, offsets). */
#include "test_framework.h"
    TEST_START

    /* BEQ taken (skips the poison) and not-taken (falls through). */
    li  t0, 5
    li  t1, 5
    li  t2, 0
    beq t0, t1, 1f
    li  t2, 99
1:  CHECK 1, t2, 0
    li  t0, 5
    li  t1, 6
    li  t2, 0
    beq t0, t1, 1f
    li  t2, 7
1:  CHECK 2, t2, 7

    /* BNE taken / not-taken. */
    li  t0, 5
    li  t1, 6
    li  t2, 0
    bne t0, t1, 1f
    li  t2, 99
1:  CHECK 3, t2, 0
    li  t0, 5
    li  t1, 5
    li  t2, 0
    bne t0, t1, 1f
    li  t2, 7
1:  CHECK 4, t2, 7

    /* BLT is signed: -1 < 1 taken; 1 < -1 not taken. */
    li  t0, -1
    li  t1, 1
    li  t2, 0
    blt t0, t1, 1f
    li  t2, 99
1:  CHECK 5, t2, 0
    li  t0, 1
    li  t1, -1
    li  t2, 0
    blt t0, t1, 1f
    li  t2, 7
1:  CHECK 6, t2, 7

    /* BGE signed: 1 >= -1 taken. */
    li  t0, 1
    li  t1, -1
    li  t2, 0
    bge t0, t1, 1f
    li  t2, 99
1:  CHECK 7, t2, 0

    /* BLTU is unsigned: 1 < 0xffffffff taken, but 0xffffffff < 1 not taken
     * (the opposite of the signed result). */
    li   t0, 1
    li   t1, -1
    li   t2, 0
    bltu t0, t1, 1f
    li   t2, 99
1:  CHECK 8, t2, 0
    li   t0, -1
    li   t1, 1
    li   t2, 0
    bltu t0, t1, 1f
    li   t2, 7
1:  CHECK 9, t2, 7

    /* BGEU unsigned: 0xffffffff >= 1 taken. */
    li   t0, -1
    li   t1, 1
    li   t2, 0
    bgeu t0, t1, 1f
    li   t2, 99
1:  CHECK 10, t2, 0

    /* Backward branch: sum 1..5 with a loop exercises a negative offset. */
    li  t0, 0                /* sum */
    li  t1, 1                /* i   */
2:  add  t0, t0, t1
    addi t1, t1, 1
    li   t3, 6
    blt  t1, t3, 2b          /* while i < 6 */
    CHECK 11, t0, 15

    TEST_END
