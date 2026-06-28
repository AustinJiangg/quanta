/*
 * M16 teaching kernel — the assembly glue around the C kernel (kernel.c).
 *
 * Three pieces live here, all of which must be assembly:
 *
 *   _start       The M-mode entry. Quanta's loader enters an ELF in Machine mode
 *                with a0 = hart id and a1 = the device-tree pointer, the way real
 *                firmware would. We act as the last sliver of firmware: delegate
 *                the traps the kernel handles to Supervisor mode, leave mtvec 0
 *                (so the kernel's own SBI ecalls still reach Quanta's firmware),
 *                set up a stack, and mret into kmain running in S-mode — a0/a1
 *                preserved as the C arguments.
 *
 *   trap_entry   The S-mode trap vector (installed in stvec by kmain). It saves
 *                the interrupted context to a frame on a dedicated kernel stack
 *                (reached through sscratch), calls trap_handler(frame), then
 *                restores and sret's back. Every trap the kernel takes comes from
 *                user mode, so the sscratch swap always exchanges the user sp for
 *                the kernel sp.
 *
 *   enter_user   The one-way trip into user mode: set sepc, arrange sret to
 *                return to U-mode with interrupts enabled, point sp at the user
 *                stack, and sret. Control only ever comes back via a trap.
 *
 * Built with -march=rv32imac for the kernel image; see the Makefile.
 */

    .equ MSTATUS_MPP,  0x1800      /* mstatus.MPP field (bits 12:11)        */
    .equ MPP_S,        0x0800      /* MPP = 01 = Supervisor                 */
    .equ SSTATUS_SPP,  0x0100      /* sstatus.SPP (bit 8): 0 -> return to U */
    .equ SSTATUS_SPIE, 0x0020      /* sstatus.SPIE (bit 5): SIE after sret  */

    /* Delegate to S-mode exactly what the kernel handles: U-mode ecall (cause 8)
     * and the three page faults (12/13/15). Deliberately NOT ecall-from-S
     * (cause 9 — that is the kernel's own SBI path, which must reach M-mode), nor
     * illegal/breakpoint, so a kernel bug surfaces through Quanta's SEE as a
     * non-zero exit instead of silently re-entering the handler. */
    .equ MEDELEG_VAL,  0xb100      /* (1<<8)|(1<<12)|(1<<13)|(1<<15) */
    .equ MIDELEG_STIP, 0x0020      /* delegate the supervisor timer (bit 5) */

/* ------------------------------------------------------------------ _start */
    .section .text.boot
    .globl _start
_start:
    la    sp, boot_stack_top       /* a real stack for the C kernel */

    li    t0, MEDELEG_VAL
    csrw  medeleg, t0
    li    t0, MIDELEG_STIP
    csrw  mideleg, t0

    /* mret into kmain, in Supervisor mode (mstatus.MPP = S). a0/a1 untouched. */
    la    t0, kmain
    csrw  mepc, t0
    csrr  t0, mstatus
    li    t1, MSTATUS_MPP
    not   t1, t1
    and   t0, t0, t1
    li    t1, MPP_S
    or    t0, t0, t1
    csrw  mstatus, t0
    mret

/* -------------------------------------------------------------- trap_entry */
    .align 4
    .globl trap_entry
trap_entry:
    /* Swap in the kernel trap stack; sscratch holds its top between traps and
     * comes back holding the user sp. */
    csrrw sp, sscratch, sp
    addi  sp, sp, -128             /* 32-word trap frame (x0 slot unused) */
    sw    x1,   1*4(sp)
    sw    x3,   3*4(sp)
    sw    x4,   4*4(sp)
    sw    x5,   5*4(sp)
    sw    x6,   6*4(sp)
    sw    x7,   7*4(sp)
    sw    x8,   8*4(sp)
    sw    x9,   9*4(sp)
    sw    x10, 10*4(sp)
    sw    x11, 11*4(sp)
    sw    x12, 12*4(sp)
    sw    x13, 13*4(sp)
    sw    x14, 14*4(sp)
    sw    x15, 15*4(sp)
    sw    x16, 16*4(sp)
    sw    x17, 17*4(sp)
    sw    x18, 18*4(sp)
    sw    x19, 19*4(sp)
    sw    x20, 20*4(sp)
    sw    x21, 21*4(sp)
    sw    x22, 22*4(sp)
    sw    x23, 23*4(sp)
    sw    x24, 24*4(sp)
    sw    x25, 25*4(sp)
    sw    x26, 26*4(sp)
    sw    x27, 27*4(sp)
    sw    x28, 28*4(sp)
    sw    x29, 29*4(sp)
    sw    x30, 30*4(sp)
    sw    x31, 31*4(sp)
    csrr  t0, sscratch             /* the user sp parked in sscratch */
    sw    t0,  2*4(sp)             /* frame[x2] = user sp */

    mv    a0, sp                   /* trap_handler(uint32_t *frame) */
    call  trap_handler

    /* Reset sscratch to the kernel trap-stack top for the next trap, then
     * restore the context (user sp last, since it overwrites our frame ptr). */
    la    t0, trap_stack_top
    csrw  sscratch, t0
    lw    x1,   1*4(sp)
    lw    x3,   3*4(sp)
    lw    x4,   4*4(sp)
    lw    x5,   5*4(sp)
    lw    x6,   6*4(sp)
    lw    x7,   7*4(sp)
    lw    x8,   8*4(sp)
    lw    x9,   9*4(sp)
    lw    x10, 10*4(sp)
    lw    x11, 11*4(sp)
    lw    x12, 12*4(sp)
    lw    x13, 13*4(sp)
    lw    x14, 14*4(sp)
    lw    x15, 15*4(sp)
    lw    x16, 16*4(sp)
    lw    x17, 17*4(sp)
    lw    x18, 18*4(sp)
    lw    x19, 19*4(sp)
    lw    x20, 20*4(sp)
    lw    x21, 21*4(sp)
    lw    x22, 22*4(sp)
    lw    x23, 23*4(sp)
    lw    x24, 24*4(sp)
    lw    x25, 25*4(sp)
    lw    x26, 26*4(sp)
    lw    x27, 27*4(sp)
    lw    x28, 28*4(sp)
    lw    x29, 29*4(sp)
    lw    x30, 30*4(sp)
    lw    x31, 31*4(sp)
    lw    x2,   2*4(sp)            /* user sp (replaces the frame pointer) */
    sret

/* -------------------------------------------------------------- enter_user */
    .align 4
    .globl enter_user
enter_user:                        /* enter_user(a0 = entry VA, a1 = user sp) */
    csrw  sepc, a0
    li    t0, SSTATUS_SPP
    csrc  sstatus, t0              /* SPP = 0  -> sret returns to U-mode      */
    li    t0, SSTATUS_SPIE
    csrs  sstatus, t0              /* SPIE = 1 -> interrupts on after sret    */
    mv    sp, a1                   /* the user stack pointer */
    li    a0, 0                    /* hand the user a tidy register file */
    li    a1, 0
    sret

/* ------------------------------------------------------------------ stacks */
    .section .bss
    .align 16
boot_stack:
    .skip 8192
    .globl boot_stack_top
boot_stack_top:
trap_stack:
    .skip 4096
    .globl trap_stack_top
trap_stack_top:
