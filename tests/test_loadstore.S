/* RV32I conformance: LOAD/STORE (widths, sign vs zero extension, endianness). */
#include "test_framework.h"
    TEST_START

    la t3, buf

    /* SW then LW round-trips a full word. */
    li t0, 0x12345678
    sw t0, 0(t3)
    lw t1, 0(t3)
    CHECK 1, t1, 0x12345678

    /* SB stores the low byte; LBU zero-extends, LB sign-extends. */
    li  t0, 0x80
    sb  t0, 4(t3)
    lbu t1, 4(t3)
    CHECK 2, t1, 0x80
    lb  t1, 4(t3)
    CHECK 3, t1, 0xffffff80

    /* SH stores the low half; LHU zero-extends, LH sign-extends. */
    li  t0, 0x8000
    sh  t0, 8(t3)
    lhu t1, 8(t3)
    CHECK 4, t1, 0x8000
    lh  t1, 8(t3)
    CHECK 5, t1, 0xffff8000

    /* Little-endian: the low byte of a stored word is at the lowest address. */
    li  t0, 0xaabbccdd
    sw  t0, 12(t3)
    lbu t1, 12(t3)
    CHECK 6, t1, 0xdd
    lbu t1, 13(t3)
    CHECK 7, t1, 0xcc
    lbu t1, 14(t3)
    CHECK 8, t1, 0xbb
    lbu t1, 15(t3)
    CHECK 9, t1, 0xaa

    TEST_END

    .section .data
buf:
    .space 16
