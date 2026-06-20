/* Pipeline demo (fast): the same array sum as tests/hazard_slow.S, computing
 * the identical result, but with the inner loop reordered so the loaded value
 * is used three instructions after the `lw` instead of right after it. That
 * separation hides the load-use latency, so a 5-stage pipeline with full
 * forwarding takes no load-use stall — fewer cycles for the same work.
 *
 * Built with the base -march=rv32i. Exits 0 iff the sum is correct. */
#include "test_framework.h"

    .equ N, 256

    .bss
    .align 2
arr:
    .skip   N * 4

    TEST_START

    /* fill arr[i] = i (identical to the slow version) */
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

    /* sum with the load scheduled away from its use: the independent pointer
     * and counter bumps fill the load-delay slot, so the dependent add no
     * longer stalls. Same instructions as the slow loop, reordered. */
    li    t0, 0            /* sum */
    mv    t2, a0           /* ptr */
    li    t1, 0            /* i   */
3:
    lw    t3, 0(t2)
    addi  t2, t2, 4        /* independent of the load */
    addi  t1, t1, 1        /* independent of the load */
    add   t0, t0, t3       /* use is now three instructions after the load */
    blt   t1, a1, 3b

    CHECK 1, t0, (N * (N - 1)) / 2
    TEST_END
