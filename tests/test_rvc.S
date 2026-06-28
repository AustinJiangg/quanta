/* M11: RV32C compressed instructions.
 *
 * Each compressed instruction is a 16-bit shorthand for a 32-bit one; Quanta
 * runs it by expanding it to that 32-bit instruction (src/rvc.c). This test
 * exercises the expansions' *semantics* — it computes with explicit c.*
 * instructions and checks the results — so a wrong immediate scramble or a
 * mis-built field is caught. The three-bit register forms (c.sub, c.lw, c.beqz,
 * ...) require x8..x15, so this uses s0/s1/a0..a5 for them.
 *
 * Pure user-mode integer work, so it is differential-tested against qemu (which
 * also implements C). Assemble with -march=rv32ic (the Makefile overrides
 * RVCFLAGS for this ELF). */
#include "test_framework.h"

    TEST_START

    /* ---- C.LI / C.ADDI / C.MV / C.ADD ---- */
    c.li    a0, 5
    CHECK 1, a0, 5
    c.li    a1, -3
    CHECK 2, a1, -3
    c.addi  a0, 7                /* 5 + 7 = 12 */
    CHECK 3, a0, 12
    c.mv    a2, a0
    CHECK 4, a2, 12
    c.add   a2, a1               /* 12 + (-3) = 9 */
    CHECK 5, a2, 9

    /* ---- C.SUB / C.AND / C.OR / C.XOR (x8..x15) ---- */
    li      s0, 20
    li      s1, 8
    c.sub   s0, s1               /* 12 */
    CHECK 6, s0, 12
    li      s0, 0xF0
    li      s1, 0x3C
    c.and   s0, s1               /* 0x30 */
    CHECK 7, s0, 0x30
    li      s0, 0xF0
    c.or    s0, s1               /* 0xFC */
    CHECK 8, s0, 0xFC
    li      s0, 0xF0
    c.xor   s0, s1               /* 0xCC */
    CHECK 9, s0, 0xCC

    /* ---- C.ANDI / C.SRLI / C.SRAI / C.SLLI ---- */
    li      s0, 0xFF
    c.andi  s0, 0x0F
    CHECK 10, s0, 0x0F
    li      s0, 0x80
    c.srli  s0, 3                /* 0x10 */
    CHECK 11, s0, 0x10
    li      s0, -16              /* 0xFFFFFFF0 */
    c.srai  s0, 2                /* arithmetic -> 0xFFFFFFFC = -4 */
    CHECK 12, s0, -4
    c.li    a0, 1
    c.slli  a0, 4                /* 16 */
    CHECK 13, a0, 16

    /* ---- C.LUI ---- */
    c.lui   a0, 2                /* 2 << 12 = 0x2000 */
    CHECK 14, a0, 0x2000

    /* ---- C.ADDI16SP / C.ADDI4SPN (stack-pointer arithmetic) ---- */
    mv      t0, sp               /* save sp */
    c.addi16sp sp, -32
    c.addi4spn s0, sp, 8         /* s0 = sp + 8 */
    sub     s1, s0, sp
    CHECK 15, s1, 8
    mv      sp, t0               /* restore sp */

    /* ---- C.SW / C.LW (base in x8..x15) ---- */
    la      s0, buf
    li      s1, 0x12345678
    c.sw    s1, 0(s0)
    c.lw    a0, 0(s0)
    CHECK 16, a0, 0x12345678

    /* ---- C.SWSP / C.LWSP (x2-relative) ---- */
    li      a0, 0xCAFEBABE
    c.swsp  a0, 0(sp)
    c.lwsp  a1, 0(sp)
    CHECK 17, a1, 0xCAFEBABE

    /* ---- C.BEQZ taken / not taken ---- */
    li      s0, 0
    li      a0, 0
    c.beqz  s0, 1f               /* taken */
    li      a0, 99
1:  CHECK 18, a0, 0
    li      s0, 5
    li      a0, 0
    c.beqz  s0, 2f               /* not taken */
    li      a0, 7
2:  CHECK 19, a0, 7

    /* ---- C.BNEZ taken ---- */
    li      s0, 7
    li      a0, 0
    c.bnez  s0, 3f               /* taken */
    li      a0, 99
3:  CHECK 20, a0, 0

    /* ---- C.J ---- */
    li      a0, 0
    c.j     4f
    li      a0, 99
4:  CHECK 21, a0, 0

    /* ---- C.JAL / C.JR (call and return via compressed jumps) ---- */
    c.jal   func                 /* ra = return address; func sets a0 = 21 */
    CHECK 22, a0, 21

    TEST_END

/* func: set a0 = 21 and return via C.JR ra (c.li's immediate is 6-bit signed). */
func:
    c.li    a0, 21
    c.jr    ra

    .bss
    .align 2
buf:
    .skip 8
