/* Enabler (M15 follow-up): supervisor timer interrupts via the SBI.
 *
 * A supervisor OS keeps time by calling SBI set_timer and taking a *supervisor*
 * timer interrupt (STIP, scause 0x80000005) when the deadline passes. The CLINT
 * itself only drives the *machine* timer (MTIP); the firmware relays it. Quanta
 * plays that firmware: when an SBI-armed deadline is reached it raises STIP,
 * which an S-mode OS takes at stvec once it has delegated and enabled it.
 *
 * This is the bare-metal boot shim plus a tiny tick loop:
 *   - M-mode prologue: delegate the supervisor timer to S-mode (mideleg),
 *     install stvec, leave mtvec 0 (so S-mode ecalls still reach the SBI), and
 *     mret into Supervisor mode.
 *   - S-mode: enable sstatus.SIE and sie.STIE, arm the first deadline, and spin
 *     until TICKS supervisor timer interrupts have fired; each handler counts
 *     the tick and re-arms via SBI set_timer. Then shut down via SRST (exit 0).
 *
 * A broken relay never fires STIP, so the spin guard trips and the program ebreaks
 * to a non-zero exit. Uses M-mode CSRs + mret, so -march=rv32i_zicsr and out of
 * make check-diff; make check pins the clean exit.
 */
#include "test_framework.h"

    .equ SBI_EXT_TIME,  0x54494D45     /* "TIME" */
    .equ SBI_EXT_SRST,  0x53525354     /* "SRST" */
    .equ MIDELEG_STIP,  0x20           /* delegate supervisor timer (bit 5) */
    .equ SSTATUS_SIE,   0x2            /* sstatus.SIE (bit 1) */
    .equ SIE_STIE,      0x20           /* sie.STIE   (bit 5) */
    .equ MSTATUS_MPP,   0x1800
    .equ MPP_S,         0x800
    .equ SCAUSE_STIMER, 0x80000005     /* interrupt bit | supervisor timer (5) */
    .equ CLINT_MTIME,   0x0200bff8     /* read the timebase the comparator uses */
    .equ DELTA,         50             /* ticks between deadlines */
    .equ TICKS,         3              /* how many interrupts to require */
    .equ SPIN_MAX,      0x100000       /* fail fast if the relay never fires */

    TEST_START                         /* M-mode; gp = 0 */

    /* delegate the supervisor timer interrupt to S-mode */
    li    t0, MIDELEG_STIP
    csrs  mideleg, t0
    /* install the S-mode trap vector */
    la    t0, s_trap
    csrw  stvec, t0
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
    call  arm_timer                    /* arm the first deadline */
1:  li    t0, TICKS
    bge   s0, t0, 2f                   /* enough ticks -> done */
    addi  s2, s2, 1
    li    t1, SPIN_MAX
    bge   s2, t1, s_fail               /* relay never fired -> fail fast */
    j     1b
2:  /* success: shut the machine down via SRST */
    li    a7, SBI_EXT_SRST
    li    a6, 0
    li    a0, 0
    li    a1, 0
    ecall
s_fail:
    ebreak                             /* no handler -> SEE stops, non-zero status */

/* arm_timer: deadline = current mtime + DELTA, set via SBI set_timer. Reads the
 * CLINT mtime directly (the counter the comparator uses); clobbers t0/t1 and the
 * a-registers, none of which the caller needs preserved. */
arm_timer:
    li    t0, CLINT_MTIME
    lw    t1, 0(t0)                    /* current mtime (low word) */
    addi  t1, t1, DELTA
    mv    a0, t1                       /* stime_value low  */
    li    a1, 0                        /* stime_value high */
    li    a7, SBI_EXT_TIME
    li    a6, 0
    ecall
    ret

    .align 2
/* Supervisor trap handler: count the timer tick and re-arm for the next one. */
s_trap:
    csrr  t0, scause
    li    t1, SCAUSE_STIMER
    bne   t0, t1, 1f                   /* only the supervisor timer is expected */
    addi  s0, s0, 1
    call  arm_timer                    /* re-arm (clears STIP, sets next deadline) */
1:  sret
