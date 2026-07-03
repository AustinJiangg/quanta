/* RV64C explicit-encoding test: the compressed slots whose meaning differs from
 * RV32C (C.ADDIW, C.LD/C.SD, C.LDSP/C.SDSP, C.SUBW/C.ADDW, 6-bit shifts). */
#include "test_framework.h"
    TEST_START

    li   a0, 0x7fffffff
    c.addiw a0, 1                  /* RV64 slot (C.JAL on RV32) -> 0x80000000 sext */
    CHECK 1, a0, 0xffffffff80000000

    li   s0, 1
    c.slli s0, 40                  /* 6-bit shamt */
    CHECK 2, s0, 0x10000000000
    li   a1, -1
    c.srli a1, 60
    CHECK 3, a1, 0xf
    li   a2, -1
    c.srai a2, 60
    CHECK 4, a2, -1

    la   a0, scratch
    li   a1, 0x1122334455667788
    c.sd a1, 0(a0)                 /* RV64 slot (C.FSW on RV32) */
    c.ld a2, 0(a0)                 /* RV64 slot (C.FLW on RV32) */
    CHECK 5, a2, 0x1122334455667788

    li   a4, 0
    li   a3, 1
    c.subw a4, a3                  /* (0-1) word -> sext = -1 */
    CHECK 6, a4, -1
    li   a4, 0x80000000
    li   a3, 0x80000000
    c.addw a4, a3                  /* word add overflows to 0 */
    CHECK 7, a4, 0

    addi sp, sp, -16
    li   a0, 0x55
    c.sdsp a0, 0(sp)              /* RV64 slot (C.FSWSP on RV32) */
    c.ldsp a1, 0(sp)             /* RV64 slot (C.FLWSP on RV32) */
    CHECK 8, a1, 0x55
    addi sp, sp, 16

    TEST_END
    .section .data
    .align 3
scratch:
    .dword 0
