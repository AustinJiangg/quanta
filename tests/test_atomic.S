/* M10: RV32A atomics — the AMO read-modify-writes and the LR/SC pair.
 *
 * Operates on a single word of stack scratch (16-byte aligned, so the atomics'
 * natural-alignment requirement holds). Each AMO check confirms both the old
 * value it returns and the new value it leaves in memory; the min/max checks
 * pin the signed-vs-unsigned split. LR/SC is checked both ways: a clean pair
 * succeeds (rd = 0) and stores, and a pair whose word is overwritten in between
 * fails (rd = 1) and does not store.
 *
 * Unlike the privileged tests these are user-mode instructions, so qemu-riscv32
 * differential-tests them too. Assemble with -march=rv32ia (Makefile override).
 *
 * The failure case overwrites the reserved word with a *different* value, so a
 * value-based reservation model (qemu-user) and our address-based one agree. */
#include "test_framework.h"
    TEST_START

    addi  sp, sp, -16           /* a word of atomic scratch at 0(s0) */
    mv    s0, sp                /* s0 = &var; callee-saved, survives CHECKs */

    /* --- AMOSWAP.W: returns the old value, installs rs2 --- */
    li    t0, 10
    sw    t0, 0(s0)
    li    t1, 20
    amoswap.w t2, t1, (s0)
    CHECK 1, t2, 10            /* old */
    lw    t3, 0(s0)
    CHECK 2, t3, 20            /* new */

    /* --- AMOADD.W --- */
    li    t1, 5
    amoadd.w t2, t1, (s0)
    CHECK 3, t2, 20            /* old */
    lw    t3, 0(s0)
    CHECK 4, t3, 25            /* 20 + 5 */

    /* --- AMOXOR.W / AMOOR.W / AMOAND.W --- */
    li    t0, 0xff
    sw    t0, 0(s0)
    li    t1, 0x0f
    amoxor.w t2, t1, (s0)
    lw    t3, 0(s0)
    CHECK 5, t3, 0xf0

    li    t0, 0xf0
    sw    t0, 0(s0)
    li    t1, 0x0f
    amoor.w t2, t1, (s0)
    lw    t3, 0(s0)
    CHECK 6, t3, 0xff

    li    t0, 0xff
    sw    t0, 0(s0)
    li    t1, 0x0f
    amoand.w t2, t1, (s0)
    lw    t3, 0(s0)
    CHECK 7, t3, 0x0f

    /* --- AMOMIN/MAX: signed vs unsigned, with var = -1 and rs2 = 1 --- */
    li    t0, -1
    sw    t0, 0(s0)
    li    t1, 1
    amomin.w t2, t1, (s0)      /* signed min(-1, 1) = -1 */
    lw    t3, 0(s0)
    CHECK 8, t3, -1

    li    t0, -1
    sw    t0, 0(s0)
    amomax.w t2, t1, (s0)      /* signed max(-1, 1) = 1 */
    lw    t3, 0(s0)
    CHECK 9, t3, 1

    li    t0, -1
    sw    t0, 0(s0)
    li    t1, 1
    amominu.w t2, t1, (s0)     /* unsigned min(0xffffffff, 1) = 1 */
    lw    t3, 0(s0)
    CHECK 10, t3, 1

    li    t0, -1
    sw    t0, 0(s0)
    amomaxu.w t2, t1, (s0)     /* unsigned max(0xffffffff, 1) = 0xffffffff */
    lw    t3, 0(s0)
    CHECK 11, t3, -1

    /* --- LR/SC success: nothing disturbs the reservation, so SC stores --- */
    li    t0, 100
    sw    t0, 0(s0)
    lr.w  t1, (s0)
    CHECK 12, t1, 100
    li    t2, 200
    sc.w  t3, t2, (s0)
    CHECK 13, t3, 0           /* 0 = success */
    lw    t4, 0(s0)
    CHECK 14, t4, 200

    /* --- LR/SC failure: an intervening store breaks the reservation --- */
    lr.w  t1, (s0)
    li    t2, 7
    sw    t2, 0(s0)           /* writes the reserved word -> reservation lost */
    li    t2, 999
    sc.w  t3, t2, (s0)
    CHECK 15, t3, 1           /* 1 = failure */
    lw    t4, 0(s0)
    CHECK 16, t4, 7           /* the 999 store did not happen */

    TEST_END
