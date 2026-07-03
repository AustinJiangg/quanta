/* M18 (xv6 enabler): supervisor timer interrupts via the Sstc extension.
 *
 * Sstc lets an S-mode OS keep time with no firmware round-trip: once M-mode sets
 * menvcfg.STCE, writing the stimecmp CSR arms a supervisor timer interrupt (STIP)
 * that fires on its own when time >= stimecmp — no SBI set_timer, no M-mode relay
 * (contrast test_stimer.S, which drives the same STIP through the SBI). This is
 * exactly how xv6 booted with -bios none drives its scheduler tick.
 *
 * Boot shim + tick loop:
 *   - M-mode prologue: enable Sstc (menvcfg.STCE), let S-mode read time
 *     (mcounteren.TM), delegate the supervisor timer (mideleg), install stvec,
 *     arm the first deadline (stimecmp = time + DELTA), and mret into S-mode.
 *   - S-mode: enable sstatus.SIE and sie.STIE, then spin until TICKS supervisor
 *     timer interrupts have fired; each handler counts the tick and re-arms
 *     stimecmp (which clears STIP until the counter catches up again). Then shut
 *     down via SBI SRST (a clean exit 0).
 *
 * A broken Sstc never fires STIP, so the spin guard trips and the program ebreaks
 * to a non-zero exit. Uses M-mode CSRs + Sstc, so it is quanta-only (out of the
 * check-rv64 qemu differential); the clean exit is pinned by quanta-exit-0.
 * The handler returns via sret (no fixed mepc bump), so it keeps C. */
#include "test_framework.h"

    .equ SBI_EXT_SRST,  0x53525354     /* "SRST" */
    .equ MCOUNTEREN_TM, 0x2            /* mcounteren.TM (bit 1): S may read time */
    .equ MIDELEG_STIP,  0x20           /* delegate supervisor timer (bit 5) */
    .equ SSTATUS_SIE,   0x2            /* sstatus.SIE (bit 1) */
    .equ SIE_STIE,      0x20           /* sie.STIE   (bit 5) */
    .equ MSTATUS_MPP,   0x1800
    .equ MPP_S,         0x800
    .equ DELTA,         100            /* counter ticks between deadlines */
    .equ TICKS,         3              /* how many interrupts to require */
    .equ SPIN_MAX,      0x100000       /* fail fast if Sstc never fires */

    TEST_START                         /* M-mode; gp = 0 */

    /* enable the Sstc extension: menvcfg.STCE (bit 63) */
    li    t0, 1
    slli  t0, t0, 63
    csrs  menvcfg, t0
    /* let supervisor mode read the time CSR */
    li    t0, MCOUNTEREN_TM
    csrs  mcounteren, t0
    /* delegate the supervisor timer interrupt to S-mode */
    li    t0, MIDELEG_STIP
    csrs  mideleg, t0
    /* install the S-mode trap vector */
    la    t0, s_trap
    csrw  stvec, t0
    /* arm the first deadline while still in M-mode (as xv6's timerinit does) */
    call  arm_timer
    /* hand off to Supervisor mode at s_main */
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
    li    s0, 0                        /* ticks taken */
    li    s2, 0                        /* spin guard */
    li    t0, SSTATUS_SIE              /* unmask supervisor interrupts */
    csrs  sstatus, t0
    li    t0, SIE_STIE                 /* enable the supervisor timer interrupt */
    csrs  sie, t0
1:  li    t0, TICKS
    bge   s0, t0, 2f                   /* enough ticks -> done */
    addi  s2, s2, 1
    li    t1, SPIN_MAX
    bge   s2, t1, s_fail               /* Sstc never fired -> fail fast */
    j     1b
2:  /* success: shut the machine down via SRST */
    li    a7, SBI_EXT_SRST
    li    a6, 0
    li    a0, 0
    li    a1, 0
    ecall
s_fail:
    ebreak                             /* no handler -> SEE stops, non-zero status */

/* arm_timer: stimecmp = current time + DELTA. Reads the time CSR (the same
 * counter Sstc compares against) and writes stimecmp, clearing STIP until the
 * counter catches up. Clobbers t0/ra, neither of which the callers need kept. */
arm_timer:
    rdtime t0
    addi   t0, t0, DELTA
    csrw   stimecmp, t0
    ret

    .align 2
/* Supervisor trap handler: count the timer tick and re-arm for the next one.
 * Trap entry clears sstatus.SIE, so this cannot be re-entered; but it must save
 * every temporary it touches (t0/t1/ra) because the interrupt can land between
 * any two instructions of the spin loop, which keeps live values in them. */
s_trap:
    addi  sp, sp, -24
    sd    t0, 0(sp)
    sd    t1, 8(sp)
    sd    ra, 16(sp)
    csrr  t0, scause
    li    t1, 1
    slli  t1, t1, 63                   /* SCAUSE interrupt bit (63 on RV64) */
    ori   t1, t1, 5                    /* | supervisor timer (cause 5) */
    bne   t0, t1, 1f                   /* only the supervisor timer is expected */
    addi  s0, s0, 1
    call  arm_timer                    /* re-arm (clears STIP, sets next deadline) */
1:  ld    t0, 0(sp)
    ld    t1, 8(sp)
    ld    ra, 16(sp)
    addi  sp, sp, 24
    sret
