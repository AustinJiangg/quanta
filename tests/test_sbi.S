/* M15: bare-metal Supervisor mode + SBI.
 *
 * The hart resets into Machine mode. This program does the one thing firmware
 * does before handing off to an OS — drop into Supervisor mode — and then acts
 * as a tiny S-mode kernel that reaches the machine only through the SBI, the
 * firmware interface layered on `ecall`. Because it installs no mtvec, its
 * S-mode `ecall`s fall through to Quanta-as-firmware (the built-in SBI).
 *
 * It exercises:
 *   - Base: probe_extension(SRST/TIME) and get_spec_version (discovery).
 *   - TIME: set_timer, armed far ahead so the timer stays quiet.
 *   - Console: putchar, printing "sbi ok\n" (the observable proof; pinned by
 *     make check-sbi).
 *   - SRST: system_reset(shutdown) — the clean exit (status 0), pinned by
 *     make check.
 *
 * On any unexpected SBI error it falls to `ebreak`, which (no handler) stops the
 * machine with a non-zero status, so make check reports a failure. Uses M-mode
 * CSRs and `mret`, so it assembles with -march=rv32i_zicsr and stays out of
 * make check-diff (user-mode qemu provides its own supervisor, not our SBI).
 */
#include "test_framework.h"

    .equ SBI_EXT_BASE, 0x10
    .equ SBI_EXT_TIME, 0x54494D45      /* "TIME" */
    .equ SBI_EXT_SRST, 0x53525354      /* "SRST" */
    .equ SBI_PUTCHAR,  0x01
    .equ BASE_PROBE,   3               /* base function id: probe_extension */
    .equ BASE_VERSION, 0               /* base function id: get_spec_version */
    .equ MSTATUS_MPP,  0x1800          /* mstatus.MPP field */
    .equ MPP_S,        0x800           /* MPP = 01 = Supervisor */

    TEST_START                         /* M-mode; gp = 0 */

    /* --- hand off to Supervisor mode at s_main (no mtvec: SBI handles ecall) --- */
    la    t0, s_main
    csrw  mepc, t0
    csrr  t0, mstatus
    li    t1, MSTATUS_MPP
    not   t1, t1
    and   t0, t0, t1                   /* clear MPP */
    li    t1, MPP_S
    or    t0, t0, t1                   /* MPP = Supervisor */
    csrw  mstatus, t0
    mret

    .align 2
s_main:
    /* --- Base: probe_extension(SRST) must report available --- */
    li    a7, SBI_EXT_BASE
    li    a6, BASE_PROBE
    li    a0, SBI_EXT_SRST
    ecall
    bnez  a0, s_fail                   /* error != SBI_SUCCESS */
    beqz  a1, s_fail                   /* value 0 -> not available */

    /* --- Base: probe_extension(TIME) must report available --- */
    li    a7, SBI_EXT_BASE
    li    a6, BASE_PROBE
    li    a0, SBI_EXT_TIME
    ecall
    bnez  a0, s_fail
    beqz  a1, s_fail

    /* --- Base: get_spec_version returns success and a non-zero version --- */
    li    a7, SBI_EXT_BASE
    li    a6, BASE_VERSION
    ecall
    bnez  a0, s_fail
    beqz  a1, s_fail

    /* --- TIME: arm the timer far in the future; expect success --- */
    li    a7, SBI_EXT_TIME
    li    a6, 0
    li    a0, -1                       /* stime low  = 0xffffffff */
    li    a1, -1                       /* stime high = 0xffffffff (never fires) */
    ecall
    bnez  a0, s_fail

    /* --- Console: print "sbi ok\n" one byte at a time via putchar --- */
    la    s0, msg
1:  lb    a0, 0(s0)
    beqz  a0, 2f
    li    a7, SBI_PUTCHAR
    ecall
    addi  s0, s0, 1
    j     1b
2:

    /* --- SRST: system_reset(shutdown) — the clean exit --- */
    li    a7, SBI_EXT_SRST
    li    a6, 0                        /* system_reset */
    li    a0, 0                        /* reset_type = shutdown */
    li    a1, 0                        /* reset_reason = none */
    ecall
    /* a successful shutdown never returns; reaching here is a failure */

s_fail:
    ebreak                             /* no handler -> SEE stops, non-zero status */

    .section .rodata
msg:
    .asciz "sbi ok\n"
