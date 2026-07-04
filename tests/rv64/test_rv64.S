/*
 * RV64 base-ISA conformance (M17): the instructions and semantics that only
 * exist or only differ at XLEN=64 — the *W word ops (sign-extended to 64), the
 * 6-bit shifts, LD/SD/LWU, RV64M (full-width and *W), and RV64A doublewords.
 *
 * Plain user-mode integer code, so it is differential-tested against
 * qemu-riscv64. Assembled -march=rv64ima -mabi=lp64 (no compressed: that is the
 * separate RV64C test).
 */
#include "test_framework.h"

    TEST_START

    /* ---- *W word ops: compute in 32 bits, sign-extend the result to 64 ---- */
    li   t0, 0x7fffffff
    addiw t1, t0, 1                 /* 0x80000000 -> sext */
    CHECK 1, t1, 0xffffffff80000000

    li   t0, 0x80000000
    li   t2, 0x80000000
    addw t1, t0, t2                 /* (0x80000000+0x80000000) low32 = 0 */
    CHECK 2, t1, 0

    li   t0, 0
    li   t2, 1
    subw t1, t0, t2                 /* -1 in 32 bits -> sext = -1 */
    CHECK 3, t1, -1

    li   t0, 1
    li   t2, 31
    sllw t1, t0, t2                 /* 1<<31 = 0x80000000 -> sext */
    CHECK 4, t1, 0xffffffff80000000

    li   t0, 0x80000000
    li   t2, 1
    sraw t1, t0, t2                 /* (int32)0x80000000 >> 1 = 0xc0000000 */
    CHECK 5, t1, 0xffffffffc0000000

    srlw t1, t0, t2                 /* (uint32)0x80000000 >> 1 = 0x40000000 */
    CHECK 6, t1, 0x40000000

    slliw t1, t0, 1                 /* 0x80000000<<1 low32 = 0 */
    CHECK 7, t1, 0
    sraiw t1, t0, 4                 /* (int32)0x80000000 >> 4 = 0xf8000000 */
    CHECK 8, t1, 0xfffffffff8000000
    srliw t1, t0, 4                 /* (uint32)0x80000000 >> 4 = 0x08000000 */
    CHECK 9, t1, 0x08000000

    /* ---- 64-bit shifts: 6-bit shift amount, full width ---- */
    li   t0, 1
    slli t1, t0, 40                 /* needs the 6th shamt bit */
    CHECK 10, t1, 0x10000000000
    li   t0, -1
    srli t1, t0, 60
    CHECK 11, t1, 0xf
    srai t1, t0, 60                 /* arithmetic: stays all-ones */
    CHECK 12, t1, -1

    /* ---- LD/SD and LW/LWU sign vs zero extension ---- */
    la   t3, scratch
    li   t0, 0x1122334455667788
    sd   t0, 0(t3)
    ld   t1, 0(t3)
    CHECK 13, t1, 0x1122334455667788

    li   t0, 0x1122334480000000     /* low word has bit 31 set */
    sd   t0, 0(t3)
    lw   t1, 0(t3)                  /* sign-extend the low word */
    CHECK 14, t1, 0xffffffff80000000
    lwu  t1, 0(t3)                  /* zero-extend the low word */
    CHECK 15, t1, 0x0000000080000000

    /* ---- RV64M: full-width and *W ---- */
    li   t0, 0x100000000            /* 2^32 */
    li   t1, 3
    mul  t2, t0, t1                 /* 3 * 2^32 */
    CHECK 16, t2, 0x300000000
    li   t0, 0x100000000
    li   t1, 0x100000000
    mulh t2, t0, t1                 /* high 64 of 2^64 = 1 */
    CHECK 17, t2, 1
    li   t0, 0x10000
    li   t1, 0x10000
    mulw t2, t0, t1                 /* 2^32 low32 = 0 -> sext 0 */
    CHECK 18, t2, 0
    li   t0, 100
    li   t1, 7
    divw t2, t0, t1                 /* 14 */
    CHECK 19, t2, 14
    li   t0, -100
    li   t1, 7
    remw t2, t0, t1                 /* -100 % 7 = -2 (sign of dividend) */
    CHECK 20, t2, -2

    /* ---- RV64A doublewords ---- */
    la   t3, scratch
    li   t0, 100
    sd   t0, 0(t3)
    li   t1, 5
    amoadd.d t2, t1, (t3)           /* old=100, mem=105 */
    CHECK 21, t2, 100
    ld   t4, 0(t3)
    CHECK 22, t4, 105

    li   t0, 0x55
    sd   t0, 0(t3)
    lr.d t1, (t3)
    CHECK 23, t1, 0x55
    li   t2, 0x66
    sc.d t4, t2, (t3)               /* success -> 0 */
    CHECK 24, t4, 0
    ld   t5, 0(t3)
    CHECK 25, t5, 0x66

    li   t0, 0xaa
    sd   t0, 0(t3)
    li   t1, 0xbb
    amoswap.d t2, t1, (t3)          /* old=0xaa, mem=0xbb */
    CHECK 26, t2, 0xaa
    ld   t4, 0(t3)
    CHECK 27, t4, 0xbb

    /* ---- branches compare the full XLEN, not the low 32 bits ----
     * Operands that differ only above bit 31: a low-32 truncation in the
     * comparison gets these wrong (e.g. `bgeu 0x3ffffff000, 0x4000000000` wrongly
     * taken, since the low words are 0xfffff000 vs 0), which is exactly what
     * blocked booting an OS whose page-table loops span high addresses.
     * qemu-riscv64 pins the correct behaviour. */
    li   t0, 0x3ffffff000           /* low32 = 0xfffff000 */
    li   t1, 0x4000000000           /* low32 = 0x00000000; t0 < t1 as u64 */
    li   t3, 0
    bgeu t0, t1, 1f                 /* t0 >= t1 (unsigned)? no -> fall through */
    li   t3, 1
1:  CHECK 28, t3, 1                 /* bgeu correctly not taken */
    li   t3, 0
    bltu t0, t1, 2f                 /* t0 < t1 (unsigned)? yes -> branch */
    j    3f
2:  li   t3, 1
3:  CHECK 29, t3, 1                 /* bltu correctly taken */

    li   t0, 0x100000000            /* 2^32, low32 = 0 */
    li   t1, 0x200000000            /* 2^33, low32 = 0 */
    li   t3, 0
    blt  t0, t1, 4f                 /* signed: 2^32 < 2^33 -> branch */
    j    5f
4:  li   t3, 1
5:  CHECK 30, t3, 1                 /* blt correctly taken */
    li   t3, 0
    bge  t1, t0, 6f                 /* signed: 2^33 >= 2^32 -> branch */
    j    7f
6:  li   t3, 1
7:  CHECK 31, t3, 1                 /* bge correctly taken */
    li   t3, 0
    beq  t0, t1, 8f                 /* differ above bit 31 -> not equal */
    li   t3, 1
8:  CHECK 32, t3, 1                 /* beq correctly not taken */

    TEST_END

    .section .data
    .align 3
scratch:
    .dword 0
