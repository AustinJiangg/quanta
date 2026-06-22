/* M9: machine-mode trap handling.
 *
 * Installs a handler via mtvec, then deliberately raises three different
 * exceptions — an environment call, a breakpoint, and an illegal instruction
 * (a write to a read-only CSR) — and confirms that each vectors into the
 * handler with the architecturally-correct mcause, that mepc points at the
 * trapping instruction, and that mret resumes the interrupted code. The
 * handler advances mepc past the 4-byte trapping instruction before returning,
 * the way a real handler skips an emulated/serviced instruction.
 *
 * This exercises the M9 "done when": a handler installed via mtvec catches a
 * deliberate exception, inspects mcause/mepc, and returns with mret.
 *
 * mtvec/mcause/mepc are machine-mode CSRs that user-mode qemu rejects, so the
 * Makefile keeps this ELF out of `make check-diff`; `make check` pins it.
 *
 * Assemble with -march=rv32i_zicsr (the Makefile overrides RVCFLAGS here). */
#include "test_framework.h"
    TEST_START

    /* Install the trap vector (direct mode — the handler is 4-byte aligned, so
     * the low two mode bits are zero). */
    la    t0, trap_handler
    csrw  mtvec, t0

    li    s0, 0                 /* s0 counts handler entries */

    ecall                       /* environment call from M -> cause 11 */
    mv    s4, s1                 /* s4 = the ecall cause the handler captured */

    ebreak                      /* breakpoint -> cause 3 */
    mv    s5, s1                 /* s5 = the ebreak cause */

    li    t0, 1
    csrw  instret, t0           /* write a read-only CSR -> illegal, cause 2 */
    mv    s6, s1                 /* s6 = the illegal-instruction cause */

    /* Tear down the vector so the framework's exit ecall reaches the built-in
     * SEE (which implements the exit syscall) instead of trapping. */
    csrw  mtvec, zero

    CHECK 1, s0, 3              /* the handler ran for all three exceptions */
    CHECK 2, s4, 11            /* environment call from M-mode */
    CHECK 3, s5, 3             /* breakpoint */
    CHECK 4, s6, 2             /* illegal instruction */

    /* mepc captured for the last trap must be word-aligned (it points at the
     * csrw that faulted), proving the handler saw a real return address. */
    andi  t0, s3, 3
    CHECK 5, t0, 0

    TEST_END

    /* The handler: count the entry, capture mcause and mepc, advance mepc past
     * the trapping instruction, and return. It writes only t0 and the s-regs it
     * reports through (s0/s1/s3), none of which the mainline needs preserved
     * across a trap. */
    .align 2
trap_handler:
    addi  s0, s0, 1
    csrr  s1, mcause
    csrr  s3, mepc
    addi  t0, s3, 4
    csrw  mepc, t0
    mret
