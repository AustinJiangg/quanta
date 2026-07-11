/*
 * M22: SBI HSM (hart state management). Quanta acts as the firmware; a small
 * S-mode SMP "kernel" brings its secondary harts down and up through the SBI HSM
 * extension, the way a real S-mode OS does under OpenSBI.
 *
 * All NHARTS harts enter in M-mode with a0 = hartid, then drop to S-mode at
 * s_main with no mtvec installed, so their S-mode ecalls reach Quanta's built-in
 * SBI (Quanta is the firmware). Then:
 *
 *   - every secondary (hartid != 0) parks itself with hart_stop;
 *   - hart 0 waits until each secondary reports STOPPED (hart_get_status), then
 *     hart_starts each at `worker` with opaque = its id; each worker atomically
 *     increments a shared counter and stops itself again;
 *   - hart 0 checks the counter is NHARTS-1 (every secondary ran exactly once),
 *     that each secondary is STOPPED again, that hart_get_status(0) is STARTED,
 *     and the two error cases: hart_start of an out-of-range id -> INVALID_PARAM,
 *     and of an already-started hart -> ALREADY_AVAILABLE.
 *
 * A clean pass powers the machine off via the SiFive test finisher (exit 0); a
 * failure powers off with a stage code (16..23) Quanta returns as its exit
 * status. Machine-mode CSRs + SBI + multi-hart, so quanta-only (no qemu
 * differential), run with --harts=4, and built -mno-relax so `la` on the in-RAM
 * shared counter stays PC-relative (gp is not the global pointer here).
 */

    .equ NHARTS,       4
    .equ EXT_HSM,      0x48534D
    .equ HSM_START,    0
    .equ HSM_STOP,     1
    .equ HSM_STATUS,   2
    .equ HSM_STARTED,  0
    .equ HSM_STOPPED,  1
    .equ ERR_INVALID_PARAM,     -3
    .equ ERR_ALREADY_AVAILABLE, -6
    .equ TEST_BASE,    0x00100000     /* SiFive test finisher (power-off) */
    .equ FIN_PASS,     0x5555
    .equ FIN_FAIL,     0x3333
    .equ MSTATUS_MPP,  0x1800
    .equ MPP_S,        0x800
    .equ SPIN_MAX,     0x4000000      /* fail-safe bound on the coordination waits */

    .section .text
    .globl _start
_start:
    mv    s0, a0                      /* s0 = my hart id */
    csrr  t0, mhartid
    bne   t0, s0, fail_hartid

    /* drop to S-mode at s_main (no mtvec -> SBI handles our ecalls) */
    la    t0, s_main
    csrw  mepc, t0
    csrr  t0, mstatus
    li    t1, MSTATUS_MPP
    not   t1, t1
    and   t0, t0, t1                  /* clear MPP */
    li    t1, MPP_S
    or    t0, t0, t1                  /* MPP = Supervisor */
    csrw  mstatus, t0
    mret

    .align 2
s_main:
    beqz  s0, boot_hart

    /* --- secondary: park via hart_stop; the call does not return --- */
    li    a7, EXT_HSM
    li    a6, HSM_STOP
    ecall
1:  wfi
    j     1b                          /* safety net: a stopped hart is never stepped */

    .align 2
    /* worker: where hart 0 restarts a secondary (a0 = hartid, a1 = opaque = id) */
worker:
    la    t0, counter
    li    t1, 1
    amoadd.d zero, t1, (t0)           /* counter += 1 */
    li    a7, EXT_HSM
    li    a6, HSM_STOP                /* stop again */
    ecall
2:  wfi
    j     2b

    .align 2
boot_hart:
    /* 1) wait for every secondary to report STOPPED */
    li    s1, 1                       /* s1 = current secondary id */
wait_stop1:
    li    s2, SPIN_MAX
1:  mv    a0, s1
    li    a7, EXT_HSM
    li    a6, HSM_STATUS
    ecall
    li    t1, HSM_STOPPED
    beq   a1, t1, 2f
    addi  s2, s2, -1
    beqz  s2, fail_nostop
    j     1b
2:  addi  s1, s1, 1
    li    t1, NHARTS
    blt   s1, t1, wait_stop1

    /* 2) start each secondary at `worker`, opaque = its id */
    li    s1, 1
start_loop:
    mv    a0, s1                      /* hartid    */
    la    a1, worker                  /* start_addr */
    mv    a2, s1                      /* opaque    */
    li    a7, EXT_HSM
    li    a6, HSM_START
    ecall
    bnez  a0, fail_start              /* expect SBI_SUCCESS (0) */
    addi  s1, s1, 1
    li    t1, NHARTS
    blt   s1, t1, start_loop

    /* 3) wait for counter == NHARTS-1 (every worker ran exactly once) */
    la    t0, counter
    li    t3, NHARTS - 1
    li    s2, SPIN_MAX
wait_count:
    ld    t2, (t0)
    beq   t2, t3, count_ok
    addi  s2, s2, -1
    beqz  s2, fail_count
    j     wait_count
count_ok:

    /* 4) each secondary must be STOPPED again */
    li    s1, 1
wait_stop2:
    li    s2, SPIN_MAX
1:  mv    a0, s1
    li    a7, EXT_HSM
    li    a6, HSM_STATUS
    ecall
    li    t1, HSM_STOPPED
    beq   a1, t1, 2f
    addi  s2, s2, -1
    beqz  s2, fail_restop
    j     1b
2:  addi  s1, s1, 1
    li    t1, NHARTS
    blt   s1, t1, wait_stop2

    /* 5) hart 0 itself must report STARTED */
    li    a0, 0
    li    a7, EXT_HSM
    li    a6, HSM_STATUS
    ecall
    li    t1, HSM_STARTED
    bne   a1, t1, fail_selfstat

    /* 6) hart_start of an out-of-range id -> INVALID_PARAM */
    li    a0, NHARTS
    la    a1, worker
    li    a2, 0
    li    a7, EXT_HSM
    li    a6, HSM_START
    ecall
    li    t1, ERR_INVALID_PARAM
    bne   a0, t1, fail_badid

    /* 7) hart_start of an already-started hart (self) -> ALREADY_AVAILABLE */
    li    a0, 0
    la    a1, worker
    li    a2, 0
    li    a7, EXT_HSM
    li    a6, HSM_START
    ecall
    li    t1, ERR_ALREADY_AVAILABLE
    bne   a0, t1, fail_already

    /* pass: power the machine off cleanly */
    li    t0, TEST_BASE
    li    t1, FIN_PASS
    sw    t1, (t0)
3:  wfi
    j     3b

fail_hartid:   li t1, 16; j die
fail_nostop:   li t1, 17; j die
fail_start:    li t1, 18; j die
fail_count:    li t1, 19; j die
fail_restop:   li t1, 20; j die
fail_selfstat: li t1, 21; j die
fail_badid:    li t1, 22; j die
fail_already:  li t1, 23; j die
die:
    slli  t1, t1, 16
    li    t2, FIN_FAIL
    or    t1, t1, t2
    li    t0, TEST_BASE
    sw    t1, (t0)
4:  j     4b

    .section .bss
    .align 3
counter: .space 8
