/* M12: Sv32 virtual memory.
 *
 * In Machine mode this builds a two-level page table by hand, then drops into
 * Supervisor mode with paging enabled so every fetch and data access is
 * translated. It proves three things:
 *
 *   1. Non-identity translation, via aliasing: two distinct virtual pages are
 *      mapped to one physical page (scratch_page), so a value written through
 *      one VA is read back through the other.
 *   2. Hardware A/D maintenance: the leaf PTE starts without its dirty bit, and
 *      the store through it leaves D set.
 *   3. Precise page faults: a load from an unmapped VA raises a load page fault
 *      (cause 13), which the M-mode handler catches before steering back to a
 *      Machine-mode epilogue that checks the results and exits.
 *
 * The program's code/stack live in the 4 MiB megapage at 0x80000000, which is
 * identity-mapped so Supervisor-mode execution continues seamlessly. The page
 * tables themselves are read by the walker physically, so they need no mapping.
 *
 * Uses supervisor CSRs and satp, so qemu-user can't run it; kept out of
 * make check-diff and pinned by make check. Assemble with -march=rv32i_zicsr. */
#include "test_framework.h"
    TEST_START

    /* --- root_pt[0x200]: identity megapage for [0x80000000,0x80400000) ---
       PPN[1]=0x200, flags V|R|W|X|A|D (supervisor, executable). */
    la    t0, root_pt
    li    t1, 0x200000cf
    li    t2, 0x200 * 4         /* byte offset 0x800 is out of sw's imm range */
    add   t2, t0, t2
    sw    t1, 0(t2)

    /* --- root_pt[0x100] -> leaf_pt (non-leaf PTE: V only) --- */
    la    t3, leaf_pt
    srli  t4, t3, 12
    slli  t4, t4, 10
    ori   t4, t4, 0x1
    li    t2, 0x100 * 4         /* offset 0x400 */
    add   t2, t0, t2
    sw    t4, 0(t2)

    /* --- leaf_pt[0] and leaf_pt[1] -> scratch_page, flags V|R|W (no A/D yet,
       so the hardware sets them on first use) --- */
    la    t5, scratch_page
    srli  t5, t5, 12
    slli  t5, t5, 10
    ori   t5, t5, 0x7
    sw    t5, 0(t3)
    sw    t5, 4(t3)

    sfence.vma                  /* flush stale translations before enabling */

    /* --- enable Sv32: satp = MODE(Sv32) | root_pt PPN --- */
    la    t0, root_pt
    srli  t0, t0, 12
    li    t1, 0x80000000
    or    t0, t0, t1
    csrw  satp, t0

    /* --- install the M-mode trap handler --- */
    la    t0, m_handler
    csrw  mtvec, t0

    /* --- mret into Supervisor mode at s_entry (mstatus.MPP = S) --- */
    la    t0, s_entry
    csrw  mepc, t0
    csrr  t1, mstatus
    li    t2, 0x1800            /* MPP field */
    not   t2, t2
    and   t1, t1, t2
    li    t2, 0x800            /* MPP = 01 = Supervisor */
    or    t1, t1, t2
    csrw  mstatus, t1
    mret

    /* --- Supervisor mode: paging is live for fetches and data --- */
    .align 2
s_entry:
    li    t0, 0x40000000        /* VA1 -> scratch_page */
    li    t1, 0xabcd1234
    sw    t1, 0(t0)             /* store sets D on leaf_pt[0] */
    li    t2, 0x40001000        /* VA2 -> scratch_page (alias of VA1's frame) */
    lw    s4, 0(t2)             /* read the value back through the other mapping */
    li    t3, 0x10000000        /* unmapped VA */
    lw    t4, 0(t3)             /* -> load page fault, vectors to m_handler */

    /* --- M-mode handler: record the cause, then steer to the epilogue --- */
    .align 2
m_handler:
    csrr  s5, mcause           /* expect 13 (load page fault) */
    csrr  t0, mstatus
    li    t1, 0x1800
    or    t0, t0, t1           /* MPP = M, so mret returns to Machine mode */
    csrw  mstatus, t0
    la    t0, exit_ok
    csrw  mepc, t0
    mret

    /* --- M-mode epilogue: paging off, verify, exit --- */
    .align 2
exit_ok:
    csrw  satp, zero
    csrw  mtvec, zero
    CHECK 1, s4, 0xabcd1234     /* aliasing: VA1's store seen through VA2 */
    la    t0, leaf_pt
    lw    t1, 0(t0)
    andi  t1, t1, 0x80         /* leaf_pt[0] dirty bit */
    CHECK 2, t1, 0x80          /* the store set D in hardware */
    CHECK 3, s5, 13            /* the unmapped load raised a load page fault */
    TEST_END

    .bss
    .balign 4096
root_pt:      .skip 4096
leaf_pt:      .skip 4096
scratch_page: .skip 4096
