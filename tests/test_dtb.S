/* M14: device tree and the boot handoff.
 *
 * The loader enters the guest the way RISC-V firmware does: a0 = boot hart id,
 * a1 = the physical address of a flattened device tree (DTB) describing the
 * machine's RAM and devices. This test plays the role of a tiny bootloader: it
 * parses that DTB from a1 and recovers the machine's memory layout and a device
 * from it, instead of assuming any fixed address.
 *
 * It walks the flattened structure block token by token (BEGIN_NODE / PROP /
 * END_NODE / END), reads big-endian fields by hand, and checks:
 *
 *   1. a0 is the boot hart id (0).
 *   2. the DTB magic at a1 is 0xd00dfeed (so a1 really points at a tree).
 *   3. the header version is 17.
 *   4. the /memory node's reg range actually contains this program (_start) —
 *      i.e. we read our own RAM layout back out of the tree, and it is right.
 *   5. a uart@ device node is present (devices are described too).
 *
 * The DTB is a boot-protocol artifact Quanta supplies, which user-mode qemu does
 * not, so this is kept out of make check-diff; make check pins the exit code.
 * Plain RV32I — assembles with the default -march=rv32i.
 *
 * FDT structure-block tokens (big-endian 32-bit words):
 *   1 BEGIN_NODE  + NUL-terminated name, zero-padded to 4 bytes
 *   2 END_NODE
 *   3 PROP        + len(u32) + nameoff(u32) + value, zero-padded to 4 bytes
 *   4 NOP
 *   9 END
 * Header fields (big-endian): magic@0, off_dt_struct@8, off_dt_strings@12,
 * version@20.
 */
#include "test_framework.h"

    .equ FDT_BEGIN_NODE, 1
    .equ FDT_END_NODE,   2
    .equ FDT_PROP,       3
    .equ FDT_NOP,        4
    .equ FDT_END,        9

    TEST_START

    mv    s0, a0                 /* boot hart id */
    mv    s1, a1                 /* DTB base */

    CHECK 1, s0, 0              /* a0 = hartid = 0 */

    /* magic = be32(dtb + 0) */
    mv    a0, s1
    call  be32
    CHECK 2, a0, 0xd00dfeed

    /* version = be32(dtb + 20) */
    addi  a0, s1, 20
    call  be32
    CHECK 3, a0, 17

    /* struct cursor = dtb + be32(dtb + 8); strings = dtb + be32(dtb + 12) */
    addi  a0, s1, 8
    call  be32
    add   s2, s1, a0            /* s2 = struct-block cursor */
    addi  a0, s1, 12
    call  be32
    add   s3, s1, a0            /* s3 = strings-block base */

    li    s4, 0                 /* in_memory: walking inside a memory@ node */
    li    s5, 0                 /* /memory reg base (low address cell) */
    li    s6, 0                 /* saw a uart@ node */
    li    s8, 0                 /* /memory reg size (low size cell) */

walk:
    mv    a0, s2
    call  be32                 /* token */
    addi  s2, s2, 4
    li    t0, FDT_END
    beq   a0, t0, walk_done
    li    t0, FDT_BEGIN_NODE
    beq   a0, t0, do_begin
    li    t0, FDT_PROP
    beq   a0, t0, do_prop
    li    t0, FDT_END_NODE
    beq   a0, t0, walk          /* leaving a node: nothing to do */
    li    t0, FDT_NOP
    beq   a0, t0, walk
    li    gp, 9                 /* unknown token -> malformed tree */
    j     _fail

do_begin:
    /* s2 -> node name (NUL-terminated). Set in_memory iff it begins "memory@",
     * and latch uart-seen iff it begins "uart@". (The memory node has no child
     * nodes, so a per-node flag set here needs no clearing on END_NODE.) */
    li    s4, 0
    mv    a0, s2
    la    a1, str_memory
    call  starts_with
    beqz  a0, 1f
    li    s4, 1
1:
    mv    a0, s2
    la    a1, str_uart
    call  starts_with
    beqz  a0, 2f
    li    s6, 1
2:
    mv    a0, s2
    call  name_adv             /* a0 = padded length of the name */
    add   s2, s2, a0
    j     walk

do_prop:
    /* s2 -> len(u32), nameoff(u32), value... */
    mv    a0, s2
    call  be32
    mv    s7, a0               /* value length (survives the calls below) */
    beqz  s4, prop_adv         /* not in /memory: skip the name match */
    addi  a0, s2, 4
    call  be32                 /* nameoff */
    add   a0, s3, a0           /* -> property name in the strings block */
    la    a1, str_reg
    call  streq
    beqz  a0, prop_adv
    /* reg = <hi_addr lo_addr hi_size lo_size>; value starts at s2+8. */
    addi  a0, s2, 12           /* lo_addr cell */
    call  be32
    mv    s5, a0               /* memory base */
    addi  a0, s2, 20           /* lo_size cell */
    call  be32
    mv    s8, a0               /* memory size */
prop_adv:
    mv    a0, s7
    call  pad4
    addi  s2, s2, 8            /* skip len + nameoff */
    add   s2, s2, a0           /* skip the padded value */
    j     walk

walk_done:
    /* CHECK 4: _start lies within [base, base+size) read from the DTB. A missing
     * memory node leaves base=size=0, so the range is empty and this fails. */
    la    t0, _start
    li    t2, 0
    bltu  t0, s5, 3f           /* below base -> not contained */
    add   t1, s5, s8
    bgeu  t0, t1, 3f           /* at/above base+size -> not contained */
    li    t2, 1
3:
    CHECK 4, t2, 1
    CHECK 5, s6, 1            /* a uart@ device node was described */

    TEST_END

/* ---- helpers (leaf; touch only a0/a1 and t0-t3, never the s-regs or gp/t6) ---- */

    .align 2
/* be32(a0 = ptr) -> a0 = big-endian 32-bit word at ptr. */
be32:
    lbu   t0, 0(a0)
    lbu   t1, 1(a0)
    lbu   t2, 2(a0)
    lbu   t3, 3(a0)
    slli  t0, t0, 24
    slli  t1, t1, 16
    slli  t2, t2, 8
    or    t0, t0, t1
    or    t0, t0, t2
    or    a0, t0, t3
    ret

/* pad4(a0 = n) -> a0 = n rounded up to a multiple of 4. */
pad4:
    addi  a0, a0, 3
    andi  a0, a0, -4
    ret

/* name_adv(a0 = name ptr) -> a0 = (strlen + 1) rounded up to 4 (bytes a
 * BEGIN_NODE name occupies, including the NUL and its padding). */
name_adv:
    mv    t0, a0
1:  lbu   t1, 0(t0)
    addi  t0, t0, 1
    bnez  t1, 1b
    sub   a0, t0, a0           /* length including the NUL */
    j     pad4

/* streq(a0, a1) -> a0 = 1 if the NUL-terminated strings are equal, else 0. */
streq:
1:  lbu   t0, 0(a0)
    lbu   t1, 0(a1)
    bne   t0, t1, 2f
    beqz  t0, 3f
    addi  a0, a0, 1
    addi  a1, a1, 1
    j     1b
2:  li    a0, 0
    ret
3:  li    a0, 1
    ret

/* starts_with(a0 = str, a1 = prefix) -> a0 = 1 if str begins with prefix. */
starts_with:
1:  lbu   t1, 0(a1)
    beqz  t1, 3f               /* prefix exhausted -> match */
    lbu   t0, 0(a0)
    bne   t0, t1, 2f
    addi  a0, a0, 1
    addi  a1, a1, 1
    j     1b
2:  li    a0, 0
    ret
3:  li    a0, 1
    ret

    .section .rodata
str_memory: .asciz "memory@"
str_uart:   .asciz "uart@"
str_reg:    .asciz "reg"
