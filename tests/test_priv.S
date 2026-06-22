/* M9: privilege levels, delegation, and the S-mode trap path.
 *
 * Walks a full round trip through all three privilege modes:
 *
 *   M: install handlers, delegate ecall-from-U to S, then mret down into U.
 *   U: ecall -> medeleg routes it to S-mode (not M), entering s_handler.
 *   S: read scause, advance sepc, sret back to U (SPP was U).
 *   U: ebreak -> not delegated, so it traps up to M, entering m_handler.
 *   M: m_handler finishes the test, mret-ing to an M-mode epilogue that exits.
 *
 * This proves delegation (medeleg), S-mode trap entry (stvec/scause/sepc as
 * mstatus's SPP/SPIE are stacked), sret, and that lowering privilege with mret
 * actually runs the following code in U-mode.
 *
 * Uses machine/supervisor CSRs, so it is kept out of `make check-diff` like the
 * other privileged tests. Assemble with -march=rv32i_zicsr. */
#include "test_framework.h"
    TEST_START

    la    t0, m_handler
    csrw  mtvec, t0             /* M-mode traps -> m_handler */
    la    t0, s_handler
    csrw  stvec, t0             /* S-mode traps -> s_handler */

    li    t0, (1 << 8)
    csrw  medeleg, t0          /* delegate "ecall from U" (cause 8) to S-mode */

    li    s5, 0                 /* s5 = s_handler entry count */
    li    s6, 0                 /* s6 = m_handler entry count */

    /* Drop to U-mode: clear mstatus.MPP (-> U) and mret to u_code. */
    la    t0, u_code
    csrw  mepc, t0
    csrr  t1, mstatus
    li    t2, (3 << 11)        /* MPP field */
    not   t2, t2
    and   t1, t1, t2           /* MPP = 00 = U */
    csrw  mstatus, t1
    mret                       /* -> u_code, now in User mode */

    /* --- S-mode handler: catches the delegated U-mode ecall --- */
    .align 2
s_handler:
    addi  s5, s5, 1
    csrr  s4, scause           /* s4 = scause (expect 8: ecall from U) */
    csrr  t0, sepc
    addi  t0, t0, 4            /* step past the ecall */
    csrw  sepc, t0
    sret                       /* -> back to U-mode (SPP was U) */

    /* --- M-mode handler: catches the non-delegated U-mode ebreak --- */
    .align 2
m_handler:
    addi  s6, s6, 1
    csrw  mtvec, zero          /* restore the SEE for the final exit ecall */
    la    t0, exit_ok
    csrw  mepc, t0
    csrr  t1, mstatus
    li    t2, (3 << 11)
    or    t1, t1, t2           /* MPP = 11 = M, so mret returns to M-mode */
    csrw  mstatus, t1
    mret                       /* -> exit_ok, back in Machine mode */

    /* --- U-mode code --- */
    .align 2
u_code:
    ecall                      /* cause 8; delegated -> s_handler (S-mode) */
    ebreak                     /* cause 3; not delegated -> m_handler (M-mode) */

    /* --- M-mode epilogue: verify and exit --- */
    .align 2
exit_ok:
    CHECK 1, s5, 1             /* the S-mode handler ran exactly once */
    CHECK 2, s4, 8             /* it saw "ecall from U" */
    CHECK 3, s6, 1             /* control reached the M-mode handler after sret */
    TEST_END
