/*
 * M19: SMP multi-hart. Exercises the three things multiple harts add over one:
 * distinct hart ids, atomics that stay correct under real cross-hart contention,
 * and inter-hart interrupts (CLINT IPIs).
 *
 * All NHARTS harts enter here in M-mode with a0 = their hart id (the qemu
 * `-bios none` convention Quanta's ELF boot follows). Each hart:
 *
 *   1. checks mhartid == its boot a0 (the ids really are distinct and delivered);
 *   2. does ITERS LR/SC increments of one shared counter — under the round-robin
 *      scheduler another hart's store lands between a hart's lr.d and sc.d, so the
 *      sc.d fails and retries; the final total must still be NHARTS*ITERS, with no
 *      lost updates, which is exactly what a broken cross-hart reservation would
 *      cause (a plain ld/add/sd loop here would lose updates);
 *   3. joins a barrier (an amoadd.d arrival count) so hart 0 knows all are done.
 *
 * Then hart 0 verifies the counter and sends hart 1 an IPI by writing the CLINT's
 * per-hart msip register; hart 1, waiting with machine software interrupts
 * enabled, takes it as a real trap, clears it, and sets a flag hart 0 spins on.
 * Hart 0 then writes the SiFive test finisher (a global power-off) for a clean
 * exit 0; the other harts park until it stops them.
 *
 * A failure powers off with a FINISHER_FAIL code (16/17/18) that Quanta returns
 * as the process exit status, so check_smp.sh can tell which stage broke. It is
 * machine-mode + multi-hart + MMIO, so it is quanta-only (no qemu differential),
 * runs with --harts=4, and is built -mno-relax so `la` on its in-RAM shared words
 * stays PC-relative (gp is not the global pointer here). Stackless: no calls, so
 * the harts sharing one initial sp never matters.
 */

    .equ NHARTS,     4
    .equ ITERS,      500
    .equ CLINT_BASE, 0x02000000     /* msip[h] at CLINT_BASE + h*4 */
    .equ TEST_BASE,  0x00100000     /* SiFive test finisher (power-off) */
    .equ FIN_PASS,   0x5555         /* finisher: pass -> exit 0 */
    .equ FIN_FAIL,   0x3333         /* finisher: fail; exit code in bits [23:16] */
    .equ MSTATUS_MIE, 0x8           /* mstatus.MIE (bit 3) */
    .equ MIE_MSIE,    0x8           /* mie.MSIE   (bit 3) */
    .equ SPIN_MAX,   0x8000000      /* fail-safe bound on the IPI wait */

    .section .text
    .globl _start
_start:
    mv    s0, a0                    /* s0 = my hart id (a0 from the boot handoff) */

    /* 1) mhartid must match the boot-supplied id. */
    csrr  t0, mhartid
    bne   t0, s0, fail_hartid

    /* 2) Hart 1 alone installs the IPI trap handler and enables machine software
     *    interrupts, before any hart can send one (hart 0 sends only post-barrier). */
    li    t1, 1
    bne   s0, t1, no_ipi_setup
    la    t0, ipi_handler
    csrw  mtvec, t0                 /* direct mode: handler is 4-byte aligned */
    li    t0, MIE_MSIE
    csrs  mie, t0
    li    t0, MSTATUS_MIE
    csrs  mstatus, t0
no_ipi_setup:

    /* 3) Contended atomic increments: ITERS successful LR/SC bumps of `counter`. */
    la    t0, counter
    li    t1, ITERS
incr_loop:
1:  lr.d  t2, (t0)
    addi  t2, t2, 1
    sc.d  t3, t2, (t0)
    bnez  t3, 1b                    /* another hart's store broke us -> retry */
    addi  t1, t1, -1
    bnez  t1, incr_loop

    /* 4) Barrier: bump the arrival count, then wait for every hart to arrive. */
    la    t0, barrier
    li    t2, 1
    amoadd.d zero, t2, (t0)
    li    t1, NHARTS
barrier_wait:
    ld    t2, (t0)
    bltu  t2, t1, barrier_wait

    /* Secondary harts park; hart 0 verifies and finishes the machine. */
    bnez  s0, secondary_park

    /* 5) Hart 0: the counter must be exactly NHARTS*ITERS — no lost updates. */
    la    t0, counter
    ld    t2, (t0)
    li    t1, NHARTS * ITERS
    bne   t2, t1, fail_count

    /* 6) IPI: write hart 1's CLINT msip, then wait for its handler's flag. */
    li    t0, CLINT_BASE + 4        /* msip[1] */
    li    t1, 1
    sw    t1, (t0)
    la    t0, ipi_flag
    li    t3, SPIN_MAX
ipi_wait:
    ld    t2, (t0)
    bnez  t2, all_pass
    addi  t3, t3, -1
    bnez  t3, ipi_wait
    j     fail_ipi

all_pass:
    li    t0, TEST_BASE             /* global power-off, exit 0 */
    li    t1, FIN_PASS
    sw    t1, (t0)
2:  j     2b                        /* wait to be stopped */

secondary_park:
    wfi
    j     secondary_park

    /* Hart 1's machine software-interrupt handler: deassert the IPI and flag it. */
    .align 2
ipi_handler:
    li    t0, CLINT_BASE + 4
    sw    zero, (t0)                /* clear msip[1] (deassert the IPI) */
    la    t0, ipi_flag
    li    t1, 1
    sd    t1, (t0)
    mret

    /* Failure paths: power off with FINISHER_FAIL and a code Quanta returns. */
fail_hartid:
    li    t1, 16
    j     die
fail_count:
    li    t1, 17
    j     die
fail_ipi:
    li    t1, 18
    j     die
die:
    slli  t1, t1, 16
    li    t2, FIN_FAIL
    or    t1, t1, t2
    li    t0, TEST_BASE
    sw    t1, (t0)
3:  j     3b

    .section .bss
    .align 3
counter:  .space 8
barrier:  .space 8
ipi_flag: .space 8
