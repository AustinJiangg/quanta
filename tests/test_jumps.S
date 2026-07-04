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

    /* JALR where rd == rs1 — the `call` far-thunk pattern (auipc ra,hi;
     * jalr ra,lo(ra)) a linker emits for a call it cannot reach with a single
     * jal. The target must be computed from rs1's value BEFORE the link is
     * written to the same register; writing the link first would clobber the
     * base and jump to pc+4 (the poison below) instead of the intended target. */
    la   ra, 1f
    li   t2, 9
    jalr ra, 0(ra)           /* target = ra (old) == 1f, not the just-written link */
    li   t2, 0               /* poison: reached only if the base was clobbered */
1:  CHECK 4, t2, 9

    TEST_END

set42:
    li   t2, 42
    ret
