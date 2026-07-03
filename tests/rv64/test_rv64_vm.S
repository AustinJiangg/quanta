/* M18: Sv39 virtual memory (the RV64 analogue of the RV32 test_vm.S).
 *
 * In Machine mode this builds a *three-level* Sv39 page table by hand, then
 * drops into Supervisor mode with paging enabled so every fetch and data access
 * is translated. It proves the same three properties as the Sv32 test, but
 * through the deeper walk that only RV64 has:
 *
 *   1. Non-identity translation via a full 3-level walk, shown by aliasing: two
 *      distinct virtual pages (VA1, VA2) are mapped to one physical page
 *      (scratch_page) through root -> l1 -> l0, so a 64-bit value written
 *      through one VA is read back through the other.
 *   2. Hardware A/D maintenance: the leaf PTE starts without its dirty bit, and
 *      the store through it leaves D set.
 *   3. Precise page faults: a load from a VA whose walk reaches the last level
 *      with no leaf raises a load page fault (cause 13), caught by the M-mode
 *      handler, which then steers to a Machine-mode epilogue that checks and exits.
 *
 * The program's code/stack live in the 1 GiB gigapage at 0x80000000, identity-
 * mapped by a single leaf PTE in the root table so Supervisor execution
 * continues seamlessly; the page tables themselves are read physically by the
 * walker, so they need no mapping.
 *
 * Uses supervisor CSRs and satp, so qemu-user can't run it; kept out of the
 * check-rv64 qemu differential and pinned by quanta-exit-0. Built rv64imac_zicsr
 * (the handler sets mepc directly, so it is C-safe). */
#include "test_framework.h"
    TEST_START

    /* --- root_pt[2]: identity gigapage for [0x80000000,0xC0000000) ---
       PPN=0x80000 (1 GiB aligned), flags V|R|W|X|A|D. 8-byte PTE at offset 16. */
    la    t0, root_pt
    li    t1, 0x200000cf
    sd    t1, 16(t0)               /* root_pt[2] */

    /* --- root_pt[1] -> l1_pt (non-leaf PTE: V only) --- VA 0x40000000: VPN[2]=1 */
    la    t2, l1_pt
    srli  t2, t2, 12
    slli  t2, t2, 10
    ori   t2, t2, 0x1
    sd    t2, 8(t0)                /* root_pt[1] */

    /* --- l1_pt[0] -> l0_pt (non-leaf PTE: V only) --- VPN[1]=0 --- */
    la    t3, l1_pt
    la    t4, l0_pt
    srli  t4, t4, 12
    slli  t4, t4, 10
    ori   t4, t4, 0x1
    sd    t4, 0(t3)               /* l1_pt[0] */

    /* --- l0_pt[0] and l0_pt[1] -> scratch_page, flags V|R|W (no A/D yet, so the
       hardware sets them on first use) --- VPN[0]=0 and 1 alias one frame --- */
    la    t5, l0_pt
    la    a0, scratch_page
    srli  a0, a0, 12
    slli  a0, a0, 10
    ori   a0, a0, 0x7
    sd    a0, 0(t5)              /* l0_pt[0] -> scratch */
    sd    a0, 8(t5)              /* l0_pt[1] -> scratch (alias of l0_pt[0]'s frame) */

    sfence.vma                    /* flush stale translations before enabling */

    /* --- enable Sv39: satp = MODE(8) | root_pt PPN --- */
    la    t0, root_pt
    srli  t0, t0, 12
    li    t1, 8
    slli  t1, t1, 60              /* MODE field, satp[63:60] */
    or    t0, t0, t1
    csrw  satp, t0

    /* --- install the M-mode trap handler --- */
    la    t0, m_handler
    csrw  mtvec, t0

    /* --- mret into Supervisor mode at s_entry (mstatus.MPP = S) --- */
    la    t0, s_entry
    csrw  mepc, t0
    csrr  t1, mstatus
    li    t2, 0x1800             /* MPP field */
    not   t2, t2
    and   t1, t1, t2
    li    t2, 0x800             /* MPP = 01 = Supervisor */
    or    t1, t1, t2
    csrw  mstatus, t1
    mret

    /* --- Supervisor mode: paging is live for fetches and data --- */
    .align 2
s_entry:
    li    t0, 0x40000000         /* VA1 -> scratch_page (via root->l1->l0[0]) */
    li    t1, 0x123456789abcdef0 /* a full 64-bit payload */
    sd    t1, 0(t0)              /* store sets D on l0_pt[0] */
    li    t2, 0x40001000         /* VA2 -> scratch_page (via l0[1], same frame) */
    ld    s4, 0(t2)             /* read the value back through the other mapping */
    li    t3, 0x40002000         /* walk reaches l0_pt[2], which is unmapped */
    ld    t4, 0(t3)             /* -> load page fault, vectors to m_handler */

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
    CHECK 1, s4, 0x123456789abcdef0 /* aliasing: VA1's store seen through VA2 */
    la    t0, l0_pt
    ld    t1, 0(t0)
    andi  t1, t1, 0x80         /* l0_pt[0] dirty bit */
    CHECK 2, t1, 0x80          /* the store set D in hardware */
    CHECK 3, s5, 13            /* the unmapped load raised a load page fault */
    TEST_END

    .bss
    .balign 4096
root_pt:      .skip 4096
l1_pt:        .skip 4096
l0_pt:        .skip 4096
scratch_page: .skip 4096
