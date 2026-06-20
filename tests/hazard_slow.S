/* Pipeline demo (slow): an array sum whose inner loop carries a load-use
 * hazard. Each `lw` is immediately followed by an instruction that reads the
 * loaded register, so a 5-stage pipeline with full forwarding still stalls one
 * cycle per iteration. tests/hazard_fast.S computes the very same sum with the
 * load scheduled away from its use; `make check-pipeline` runs both under
 * --pipeline and checks that the reorder cut the stalls without changing the
 * result.
 *
 * Built with the base -march=rv32i. Exits 0 iff the sum is correct. */
#include "test_framework.h"

    .equ N, 256

    .bss
    .align 2
arr:
    .skip   N * 4

    TEST_START

    /* fill arr[i] = i */
    la    a0, arr
    li    a1, N
    li    t0, 0
1:
    bge   t0, a1, 2f
    slli  t1, t0, 2
    add   t2, a0, t1
    sw    t0, 0(t2)
    addi  t0, t0, 1
    j     1b
2:

    /* sum with a load-use hazard: the add reads t3 right after the load. */
    li    t0, 0            /* sum */
    mv    t2, a0           /* ptr */
    li    t1, 0            /* i   */
3:
    lw    t3, 0(t2)
    add   t0, t0, t3       /* <-- depends on the immediately preceding load */
    addi  t2, t2, 4
    addi  t1, t1, 1
    blt   t1, a1, 3b

    CHECK 1, t0, (N * (N - 1)) / 2
    TEST_END
