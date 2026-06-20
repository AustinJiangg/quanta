/* Stack + memory-traversal smoke test (and a small locality workload).
 *
 * This is the first program that needs a working stack. The loader points sp
 * at the top of the guest region (see main.c), and sum_arr() below — a
 * non-leaf function — spills ra and four callee-saved registers to the stack
 * across its calls to get(). If sp were left at the ISA reset value of 0, the
 * first spill would fault, so a clean run proves the stack works end to end.
 *
 * It also fills and then sums an N-element array: the kind of locality-
 * sensitive loop the future cache model (M6) will measure. Bump N to scale the
 * workload.
 *
 * Built with the base -march=rv32i (no extension needed). Exits 0 iff the sum
 * matches the closed form N*(N-1)/2, else with the failing check's id. */
#include "test_framework.h"

    .equ N, 256

    .bss
    .align 2
arr:
    .skip   N * 4                /* N words, zero-initialised (BSS) */

    TEST_START                   /* -> .text; _start:; gp = 0 */

    la    a0, arr
    li    a1, N
    jal   ra, fill               /* arr[i] = i for i in [0, N) */

    la    a0, arr
    li    a1, N
    jal   ra, sum_arr            /* a0 = sum of arr[] */
    mv    t0, a0

    CHECK 1, t0, (N * (N - 1)) / 2

    TEST_END                     /* exit(0), or exit(check id) on mismatch */

/* ---- fill(a0=base, a1=count): leaf; writes arr[i] = i. ---- */
fill:
    li    t0, 0                  /* i */
1:
    bge   t0, a1, 2f
    slli  t1, t0, 2
    add   t2, a0, t1
    sw    t0, 0(t2)
    addi  t0, t0, 1
    j     1b
2:
    ret

/* ---- get(a0=base, a1=index) -> a0 = arr[index]: leaf. ---- */
get:
    slli  t0, a1, 2
    add   t0, a0, t0
    lw    a0, 0(t0)
    ret

/* ---- sum_arr(a0=base, a1=count) -> a0 = sum.
 *      Non-leaf: it calls get() inside the loop, so it must preserve ra and
 *      keep its loop state in callee-saved registers — all spilled to the
 *      stack in the prologue and restored in the epilogue. ---- */
sum_arr:
    addi  sp, sp, -32
    sw    ra, 28(sp)
    sw    s0, 24(sp)
    sw    s1, 20(sp)
    sw    s2, 16(sp)
    sw    s3, 12(sp)
    mv    s0, a0                 /* base  */
    mv    s1, a1                 /* count */
    li    s2, 0                  /* sum   */
    li    s3, 0                  /* i     */
1:
    bge   s3, s1, 2f
    mv    a0, s0
    mv    a1, s3
    jal   ra, get               /* a0 = arr[i] (clobbers ra, t0, a0, a1) */
    add   s2, s2, a0
    addi  s3, s3, 1
    j     1b
2:
    mv    a0, s2
    lw    ra, 28(sp)
    lw    s0, 24(sp)
    lw    s1, 20(sp)
    lw    s2, 16(sp)
    lw    s3, 12(sp)
    addi  sp, sp, 32
    ret
