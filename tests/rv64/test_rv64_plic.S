/* M18 (xv6 enabler): supervisor external interrupts via the PLIC S-mode context.
 *
 * An S-mode OS (xv6, Linux) takes device interrupts in S-mode. The PLIC has a
 * per-hart S-mode context (context 1 on hart 0) with its own enable bitmap,
 * threshold, and claim/complete registers, and it drives the hart's SEIP line —
 * distinct from the M-mode context (context 0 / MEIP) that test_irq.S exercises.
 *
 * This test drops to S-mode, arms that context for the 16550 UART source, raises
 * a UART interrupt (enabling the THR-empty interrupt asserts it), and confirms
 * the supervisor external interrupt fires exactly once and the S-mode claim
 * returns the UART source — the path virtio_disk_intr / uartintr reach in a
 * booting xv6. The handler deasserts the UART and completes the claim.
 *
 * Quanta-only (M-mode CSRs + MMIO, which user-mode qemu rejects), so it is out of
 * the check-rv64 qemu differential; the clean SRST exit is pinned by quanta-exit-0.
 * Returns via sret (no fixed mepc bump), so it keeps the compressed extension. */
#include "test_framework.h"

    .equ SBI_EXT_SRST,   0x53525354    /* "SRST" */
    .equ PLIC_PRIO_UART, 0x0c000028    /* priority[10] = base + 4*10 */
    .equ PLIC_SENABLE,   0x0c002080    /* context-1 (S-mode) enable bitmap */
    .equ PLIC_STHRESH,   0x0c201000    /* context-1 threshold */
    .equ PLIC_SCLAIM,    0x0c201004    /* context-1 claim/complete */
    .equ UART_IER,       0x10000001
    .equ UART_IRQ,       10
    .equ MIDELEG_SEIP,   0x200         /* delegate supervisor external (bit 9) */
    .equ SSTATUS_SIE,    0x2           /* sstatus.SIE (bit 1) */
    .equ SIE_SEIE,       0x200         /* sie.SEIE   (bit 9) */
    .equ MSTATUS_MPP,    0x1800
    .equ MPP_S,          0x800
    .equ SPIN_MAX,       0x100000      /* fail fast if the interrupt never fires */

    TEST_START                         /* M-mode; gp = 0 */

    /* delegate the supervisor external interrupt to S-mode, install the S-mode
     * trap vector, and mret into S-mode at s_main. */
    li    t0, MIDELEG_SEIP
    csrs  mideleg, t0
    la    t0, s_trap
    csrw  stvec, t0
    la    t0, s_main
    csrw  mepc, t0
    csrr  t0, mstatus
    li    t1, MSTATUS_MPP
    not   t1, t1
    and   t0, t0, t1
    li    t1, MPP_S
    or    t0, t0, t1
    csrw  mstatus, t0
    mret

    .align 2
s_main:
    li    s0, 0                        /* external interrupts taken */
    li    s1, 0                        /* PLIC source the handler claimed */
    li    s2, 0                        /* spin guard */

    /* arm the PLIC S-mode context: the UART source above a zero threshold */
    li    t0, PLIC_PRIO_UART
    li    t1, 1
    sw    t1, 0(t0)                    /* priority[UART] = 1 */
    li    t0, PLIC_STHRESH
    sw    zero, 0(t0)                  /* S-context threshold = 0 */
    li    t0, PLIC_SENABLE
    li    t1, (1 << UART_IRQ)
    sw    t1, 0(t0)                    /* enable the UART source for the S context */

    /* unmask supervisor interrupts and the external source */
    li    t0, SSTATUS_SIE
    csrs  sstatus, t0
    li    t0, SIE_SEIE
    csrs  sie, t0

    /* raise the UART interrupt line: enabling the THR-empty interrupt asserts it */
    li    t0, UART_IER
    li    t1, 2                        /* IER.THRE */
    sb    t1, 0(t0)

1:  bnez  s0, 2f                       /* interrupt taken? */
    addi  s2, s2, 1
    li    t1, SPIN_MAX
    bge   s2, t1, s_fail               /* never fired -> fail fast */
    j     1b

2:  li    t0, 1
    bne   s0, t0, s_fail               /* exactly one external interrupt */
    li    t0, UART_IRQ
    bne   s1, t0, s_fail               /* claim returned the UART source */
    /* success: shut the machine down via SRST (a clean exit 0) */
    li    a7, SBI_EXT_SRST
    li    a6, 0
    li    a0, 0
    li    a1, 0
    ecall

s_fail:
    csrw  stvec, zero                  /* no handler -> the ebreak reaches the SEE */
    ebreak                             /* stops with a non-zero status */

    .align 2
/* Supervisor trap handler: claim the external interrupt, record its source,
 * deassert the UART, and complete. Trap entry clears sstatus.SIE, so it cannot
 * be re-entered; it saves every temporary it touches since the interrupt can
 * land between any two instructions of the spin loop. */
s_trap:
    addi  sp, sp, -24
    sd    t0, 0(sp)
    sd    t1, 8(sp)
    sd    t2, 16(sp)
    csrr  t0, scause
    li    t1, 1
    slli  t1, t1, 63                   /* SCAUSE interrupt bit (63 on RV64) */
    ori   t1, t1, 9                    /* | supervisor external (cause 9) */
    bne   t0, t1, 1f                   /* only the external interrupt is expected */
    addi  s0, s0, 1
    li    t1, PLIC_SCLAIM
    lw    t2, 0(t1)                    /* claim -> the interrupting source */
    mv    s1, t2
    li    t0, UART_IER
    sb    zero, 0(t0)                  /* deassert the UART line */
    sw    t2, 0(t1)                    /* complete the claim */
1:  ld    t0, 0(sp)
    ld    t1, 8(sp)
    ld    t2, 16(sp)
    addi  sp, sp, 24
    sret
