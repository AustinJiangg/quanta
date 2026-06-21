/* RV32I + Zicsr/Zifencei conformance: CSR access and fence.i.
 *
 * Exercises the six CSR instructions (csrrw/csrrs/csrrc and their immediate
 * forms) against mscratch — a plain read/write CSR — plus the read-only
 * counters (instret/instreth) and fence.i. The checks cover the read-modify-
 * write semantics (old value returned in rd, new value installed) and the
 * side-effect rules: a set/clear with an x0/zero source must not write.
 *
 * mscratch is nominally a machine-mode CSR; we model CSRs without privilege
 * until M9, so this runs cleanly here. User-mode qemu rejects it, which is why
 * the Makefile keeps this ELF out of `make check-diff`.
 *
 * Assemble with -march=rv32i_zicsr_zifencei (the Makefile overrides RVCFLAGS
 * for this ELF only). */
#include "test_framework.h"
    TEST_START

    /* --- mscratch round-trips (csrw/csrr are the rd-/rs1-is-x0 pseudos) --- */
    li    t0, 0x12345678
    csrw  mscratch, t0          /* mscratch = t0 */
    csrr  t1, mscratch          /* t1 = mscratch */
    CHECK 1, t1, 0x12345678

    /* --- csrrw returns the old value and installs the new one --- */
    li    t0, 0xaaaaaaaa
    csrw  mscratch, t0
    li    t1, 0x55555555
    csrrw t2, mscratch, t1      /* t2 = old (0xaaaaaaaa), mscratch = 0x55555555 */
    CHECK 2, t2, 0xaaaaaaaa
    csrr  t3, mscratch
    CHECK 3, t3, 0x55555555

    /* --- csrrs sets bits, csrrc clears bits; both return the old value --- */
    li    t0, 0xf0
    csrw  mscratch, t0
    li    t1, 0x0f
    csrrs t2, mscratch, t1      /* t2 = 0xf0, mscratch = 0xff */
    CHECK 4, t2, 0xf0
    csrr  t3, mscratch
    CHECK 5, t3, 0xff

    li    t1, 0x0f
    csrrc t2, mscratch, t1      /* t2 = 0xff, mscratch = 0xf0 */
    CHECK 6, t2, 0xff
    csrr  t3, mscratch
    CHECK 7, t3, 0xf0

    /* --- a set/clear with an x0 source reads but must not write --- */
    li    t0, 0x5a5a5a5a
    csrw  mscratch, t0
    csrrs t2, mscratch, x0      /* read only; no write */
    CHECK 8, t2, 0x5a5a5a5a
    csrr  t3, mscratch
    CHECK 9, t3, 0x5a5a5a5a

    /* --- immediate forms: 5-bit zero-extended source --- */
    csrwi mscratch, 31          /* mscratch = 31 */
    csrr  t0, mscratch
    CHECK 10, t0, 31
    csrrsi t1, mscratch, 0      /* uimm 0: read, no write */
    CHECK 11, t1, 31
    li    t2, 0
    csrw  mscratch, t2
    csrrwi t3, mscratch, 5      /* t3 = old (0), mscratch = 5 */
    CHECK 12, t3, 0
    csrr  t0, mscratch
    CHECK 13, t0, 5
    csrrci t1, mscratch, 1      /* t1 = old (5), mscratch = 4 */
    CHECK 14, t1, 5
    csrr  t0, mscratch
    CHECK 15, t0, 4

    /* --- the instret counter advances; writes to it (read-only) are dropped --- */
    csrr  t0, instret           /* X */
    nop
    nop
    csrr  t1, instret           /* X + 3 (this csrr + the two nops retired) */
    sub   t2, t1, t0
    CHECK 16, t2, 3
    csrr  t3, instreth          /* high half is still zero this early */
    CHECK 17, t3, 0

    /* --- Zifencei: fence.i is a no-op, not a halt --- */
    li    t0, 7
    fence.i
    addi  t0, t0, 1
    CHECK 18, t0, 8

    TEST_END
