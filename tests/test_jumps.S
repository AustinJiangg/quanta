/* RV32I conformance: JAL / JALR (linking, plain jumps, low-bit clearing). */
#include "test_framework.h"
    TEST_START

    /* JAL calls a subroutine; RET (jalr x0,0(ra)) returns to the caller. */
    li   t2, 0
    jal  ra, set42
    CHECK 1, t2, 42

    /* JAL with rd=x0 is a plain jump; it skips the poison instruction. */
    li   t2, 1
    jal  x0, 1f
    li   t2, 0
1:  CHECK 2, t2, 1

    /* JALR clears the low bit of the computed target address. */
    la   t0, 1f
    ori  t0, t0, 1           /* set the low bit; JALR must clear it */
    li   t2, 5
    jalr x0, 0(t0)           /* jump to (t0 & ~1) == 1f */
    li   t2, 0
1:  CHECK 3, t2, 5

    TEST_END

set42:
    li   t2, 42
    ret
