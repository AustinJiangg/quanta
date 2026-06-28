/*
 * M16 teaching kernel — the user-mode payload.
 *
 * This is the userspace process the kernel runs: it prints a line with the
 * write syscall, spins long enough to be preempted by a few supervisor timer
 * interrupts, then exits. The kernel copies this blob into a fresh physical
 * page and maps it at USER_CODE_VA, so it must be position-independent — only
 * PC-relative addressing (lla, relative branches) and immediates, no absolute
 * symbol references. The bytes are bracketed by __user_start/__user_end (see
 * kernel.ld); the entry is the first instruction.
 *
 * Syscalls follow the same RISC-V Linux/newlib ABI the kernel's trap handler
 * implements: number in a7, args in a0-a2, result in a0.
 */

    .equ SYS_write, 64
    .equ SYS_exit,  93

    .section .user, "ax"
    .globl __user_blob
__user_blob:                       /* runs at USER_CODE_VA */
    /* write(1, msg, msglen). The length is computed from two PC-relative
     * addresses rather than a label difference, so the blob stays relocatable
     * and needs no assembly-time constant. */
    li    a0, 1                    /* fd = stdout */
    lla   a1, umsg                 /* PC-relative: correct at any load base */
    lla   a2, umsg_end
    sub   a2, a2, a1               /* a2 = length */
    li    a7, SYS_write
    ecall

    /* Spin so the timer fires a few times while we are the running process.
     * The kernel caps the preemptions; we just need to run long enough. */
    li    t0, 0
    li    t1, 100000
1:  addi  t0, t0, 1
    bltu  t0, t1, 1b

    /* exit(0) */
    li    a0, 0
    li    a7, SYS_exit
    ecall

2:  j     2b                       /* the kernel shuts down on exit; unreachable */

umsg:
    .ascii "hello from userspace\n"
umsg_end:
