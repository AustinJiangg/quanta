/* RV64 privileged: M-mode trap handling + a 64-bit CSR round-trip. */
#include "test_framework.h"
    TEST_START
    la    t0, trap_handler
    csrw  mtvec, t0
    li    s0, 0

    li    t1, 0x1122334455667788    /* full 64-bit value */
    csrw  mscratch, t1
    csrr  s7, mscratch              /* RV32 would keep only the low 32 bits */

    ecall                           /* cause 11 (ECALL from M) */
    mv    s4, s1
    ebreak                          /* cause 3 */
    mv    s5, s1
    li    t0, 1
    csrw  instret, t0               /* write read-only CSR -> illegal, cause 2 */
    mv    s6, s1

    csrw  mtvec, zero               /* let the exit ecall reach the SEE */

    CHECK 1, s0, 3                  /* handler ran three times */
    CHECK 2, s4, 11
    CHECK 3, s5, 3
    CHECK 4, s6, 2
    CHECK 5, s7, 0x1122334455667788 /* 64-bit CSR round-trip */
    srli  t0, s6, 63                /* mcause interrupt bit (63) clear for an exception */
    CHECK 6, t0, 0
    andi  t0, s3, 3                 /* mepc word-aligned */
    CHECK 7, t0, 0
    TEST_END

    .align 2
trap_handler:
    addi  s0, s0, 1
    csrr  s1, mcause
    csrr  s3, mepc
    addi  t0, s3, 4
    csrw  mepc, t0
    mret
