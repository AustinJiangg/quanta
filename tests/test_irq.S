/* M13: platform devices and interrupt delivery.
 *
 * Installs an mtvec handler, then drives each of the three machine interrupts to
 * fire exactly once and confirms the handler saw it:
 *
 *   - timer:    arm the CLINT mtimecmp a little ahead of mtime, enable MTIE, and
 *               spin until the timer interrupt (mcause 0x80000007) fires; the
 *               handler disarms the comparator so it fires only once.
 *   - software: raise the CLINT msip (IPI), enable MSIE, spin until the software
 *               interrupt (0x80000003) fires; the handler clears msip.
 *   - external: give the UART (PLIC source 10) a priority above the threshold,
 *               enable it in the PLIC and enable MEIE, then assert the UART's
 *               THR-empty interrupt; spin until the external interrupt
 *               (0x8000000b) fires; the handler claims (expecting source 10),
 *               deasserts the UART, and completes.
 *
 * Finally it prints "uart ok\n" through the UART transmit register, proving the
 * MMIO console path, and exits 0. All of this is machine-mode CSR + MMIO work
 * that user-mode qemu can't run, so the Makefile keeps it out of check-diff and
 * builds it with -march=rv32i_zicsr; make check pins it.
 */
#include "test_framework.h"

    /* MMIO addresses (qemu 'virt' layout). */
    .equ CLINT_MSIP,      0x02000000
    .equ CLINT_MTIMECMP,  0x02004000
    .equ CLINT_MTIMECMPH, 0x02004004
    .equ CLINT_MTIME,     0x0200bff8
    .equ PLIC_PRIO_UART,  0x0c000028   /* priority[10] = base + 4*10 */
    .equ PLIC_ENABLE,     0x0c002000   /* context-0 enable bitmap   */
    .equ PLIC_THRESHOLD,  0x0c200000   /* context-0 threshold       */
    .equ PLIC_CLAIM,      0x0c200004   /* context-0 claim/complete  */
    .equ UART_THR,        0x10000000
    .equ UART_IER,        0x10000001
    .equ MIE_MSIE,        0x008        /* bit 3  */
    .equ MIE_MTIE,        0x080        /* bit 7  */
    .equ MIE_MEIE,        0x800        /* bit 11 */

    TEST_START

    la    t0, trap_handler
    csrw  mtvec, t0
    li    s0, 0                 /* timer interrupt count    */
    li    s1, 0                 /* software interrupt count */
    li    s2, 0                 /* external interrupt count */
    li    s3, 0                 /* PLIC source the handler claimed */

    li    t0, 0x8               /* mstatus.MIE: global machine-interrupt enable */
    csrs  mstatus, t0

    /* ---- timer interrupt ---- */
    li    t0, CLINT_MTIME
    lw    t1, 0(t0)             /* t1 = mtime (low word) */
    addi  t1, t1, 100           /* fire ~100 ticks from now */
    li    t0, CLINT_MTIMECMP
    sw    t1, 0(t0)
    li    t0, CLINT_MTIMECMPH
    sw    zero, 0(t0)
    li    t0, MIE_MTIE
    csrs  mie, t0
1:  beqz  s0, 1b               /* wait for the timer handler */
    li    t0, MIE_MTIE
    csrc  mie, t0

    /* ---- software interrupt (IPI) ---- */
    li    t0, MIE_MSIE
    csrs  mie, t0
    li    t0, CLINT_MSIP
    li    t1, 1
    sw    t1, 0(t0)            /* raise msip */
2:  beqz  s1, 2b
    li    t0, MIE_MSIE
    csrc  mie, t0

    /* ---- external interrupt via the PLIC and UART ---- */
    li    t0, PLIC_PRIO_UART
    li    t1, 1
    sw    t1, 0(t0)           /* priority[UART] = 1 */
    li    t0, PLIC_THRESHOLD
    sw    zero, 0(t0)         /* threshold = 0 */
    li    t0, PLIC_ENABLE
    li    t1, 0x400           /* enable source 10 (1 << 10) */
    sw    t1, 0(t0)
    li    t0, MIE_MEIE
    csrs  mie, t0
    li    t0, UART_IER
    li    t1, 2              /* IER.THRE: assert the UART interrupt line */
    sb    t1, 0(t0)
3:  beqz  s2, 3b
    li    t0, MIE_MEIE
    csrc  mie, t0

    /* Tear the vector down so the framework's exit ecall reaches the SEE. */
    csrw  mtvec, zero

    CHECK 1, s0, 1            /* the timer fired exactly once    */
    CHECK 2, s1, 1            /* the IPI fired exactly once       */
    CHECK 3, s2, 1            /* the external interrupt fired once */
    CHECK 4, s3, 10           /* the PLIC claim returned the UART  */

    /* Print through the UART, proving the MMIO console path. */
    la    t0, msg
4:  lb    t1, 0(t0)
    beqz  t1, 5f
    li    t2, UART_THR
    sb    t1, 0(t2)
    addi  t0, t0, 1
    j     4b
5:
    TEST_END

    /* Shared interrupt handler: dispatch on mcause. These are all interrupts, so
     * mepc points at the interrupted instruction and we resume it (no mepc
     * bump). Uses only t0-t2 and the s-regs it reports through. */
    .align 2
trap_handler:
    csrr  t0, mcause
    li    t1, 0x80000007
    beq   t0, t1, h_timer
    li    t1, 0x80000003
    beq   t0, t1, h_soft
    li    t1, 0x8000000b
    beq   t0, t1, h_ext
    mv    s7, t0               /* unexpected cause: record it, counts stay 0 */
    mret
h_timer:
    addi  s0, s0, 1
    li    t1, CLINT_MTIMECMP   /* disarm: mtimecmp = all ones */
    li    t2, -1
    sw    t2, 0(t1)
    li    t1, CLINT_MTIMECMPH
    sw    t2, 0(t1)
    mret
h_soft:
    addi  s1, s1, 1
    li    t1, CLINT_MSIP
    sw    zero, 0(t1)          /* clear the software interrupt */
    mret
h_ext:
    addi  s2, s2, 1
    li    t1, PLIC_CLAIM
    lw    t2, 0(t1)            /* claim -> the interrupting source */
    mv    s3, t2
    li    t0, UART_IER
    sb    zero, 0(t0)          /* deassert the UART line */
    sw    t2, 0(t1)            /* complete the claim */
    mret

    .section .rodata
msg:
    .asciz "uart ok\n"
