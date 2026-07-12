#include "mmu.h"
#include "decode.h"

/*
 * Virtual memory: Sv32 on RV32 (M12) and Sv39 on RV64 (M18).
 *
 * Once satp enables paging and the hart drops below Machine mode, every
 * instruction fetch and data access goes through a multi-level page-table walk
 * that turns a virtual address into a physical one. This is the layer that lets
 * each program believe it owns the whole address space: the page tables map its
 * virtual pages onto whatever physical frames the supervisor chose, and an
 * access with no valid mapping — or the wrong permission — raises a precise
 * page-fault exception instead of touching memory.
 *
 * The two schemes are the same walk at different sizes, so one loop serves both,
 * parameterised by a small descriptor:
 *
 *   Sv32:  va = | VPN[1]:10 | VPN[0]:10 | offset:12 |   2 levels, 4-byte PTEs
 *   Sv39:  va = | VPN[2]:9 | VPN[1]:9 | VPN[0]:9 | offset:12 |
 *                                                    3 levels, 8-byte PTEs
 *
 * The walk indexes the root table by the top VPN field and descends until it
 * reaches a leaf PTE; a leaf found above the last level is a superpage (a 4 MiB
 * Sv32 megapage, or a 2 MiB Sv39 megapage / 1 GiB gigapage) whose low address
 * bits come straight from the VA. A leaf PTE supplies the physical page number;
 * its low byte carries valid/permission and the accessed/dirty bits, which we
 * maintain in hardware.
 *
 * The walk is the slow path; a small TLB caches recent translations. Because a
 * stale entry would hide page-table edits, the TLB is flushed by sfence.vma and
 * by any write to satp.
 */

/* Paging schemes, selected by satp.MODE. Bare is the identity map; the two walks
 * are Sv32 (RV32) and Sv39 (RV64). An RV64 satp.MODE of 9/10 (Sv48/Sv57) is a
 * scheme Quanta does not model. */
enum { PT_BARE, PT_SV32, PT_SV39, PT_UNSUPPORTED };

/* satp.MODE: one bit (31) on RV32; four bits (63:60) on RV64, where 8 is Sv39. */
#define SATP32_MODE      0x80000000u
#define SATP64_MODE_SV39 8u

/* PTE permission/status bits (identical low byte in Sv32 and Sv39). */
enum {
    PTE_V = 1u << 0, PTE_R = 1u << 1, PTE_W = 1u << 2, PTE_X = 1u << 3,
    PTE_U = 1u << 4, PTE_G = 1u << 5, PTE_A = 1u << 6, PTE_D = 1u << 7
};

/* Which paging scheme satp currently selects. */
static int satp_mode(const CPU *cpu, uint64_t satp) {
    if (cpu->xlen == 32)
        return ((uint32_t)satp & SATP32_MODE) ? PT_SV32 : PT_BARE;
    switch ((satp >> 60) & 0xf) {
        case 0:                return PT_BARE;
        case SATP64_MODE_SV39: return PT_SV39;
        default:               return PT_UNSUPPORTED; /* Sv48/Sv57 not modelled */
    }
}

int mmu_satp_supported(const CPU *cpu, uint64_t val) {
    return satp_mode(cpu, val) != PT_UNSUPPORTED;
}

/* Which page-fault cause an access of this kind raises. */
static uint32_t pf_cause(AccessType acc) {
    return acc == ACC_FETCH ? CAUSE_INSN_PAGE_FAULT
         : acc == ACC_STORE ? CAUSE_STORE_PAGE_FAULT
         :                     CAUSE_LOAD_PAGE_FAULT;
}

/* The privilege a translation runs at. Fetches always use the current mode;
 * loads/stores use MPP instead when mstatus.MPRV is set, which is how M-mode
 * software reaches through a lower mode's mappings. */
static uint32_t eff_priv(const CPU *cpu, AccessType acc) {
    uint32_t s = cpu->csr[CSR_MSTATUS];
    if (acc != ACC_FETCH && (s & MSTATUS_MPRV))
        return (s & MSTATUS_MPP) >> MSTATUS_MPP_SHIFT;
    return cpu->priv;
}

/* Does a leaf PTE permit this access at this privilege? Covers the U-bit rules
 * (U pages are for user code; a supervisor needs SUM to touch them and can
 * never fetch from them) and the R/W/X bits (with MXR letting an execute-only
 * page be read). */
static int perm_ok(const CPU *cpu, uint32_t pte, AccessType acc, uint32_t priv) {
    uint32_t s = cpu->csr[CSR_MSTATUS];
    if (priv == PRIV_U) {
        if (!(pte & PTE_U)) return 0;
    } else { /* PRIV_S — M never reaches here, translation is off in M */
        if (pte & PTE_U) {
            if (acc == ACC_FETCH) return 0;
            if (!(s & MSTATUS_SUM)) return 0;
        }
    }
    switch (acc) {
        case ACC_FETCH: return (pte & PTE_X) != 0;
        case ACC_LOAD:  return (pte & PTE_R) || ((s & MSTATUS_MXR) && (pte & PTE_X));
        case ACC_STORE: return (pte & PTE_W) != 0;
    }
    return 0;
}

uint32_t mmu_translate(CPU *cpu, uint64_t va, AccessType acc, uint64_t *pa) {
    uint64_t satp = cpu->csr[CSR_SATP];
    uint32_t priv = eff_priv(cpu, acc);

    if (cpu->xlen == 32) va &= 0xffffffffu; /* recover a sign-extended RV32 VA */

    /* Machine mode and Bare mode access physical memory directly. (An
     * unsupported RV64 MODE cannot reach satp — csr_write drops it — but map it
     * to Bare defensively.) */
    int scheme = satp_mode(cpu, satp);
    if (priv == PRIV_M || scheme == PT_BARE || scheme == PT_UNSUPPORTED) {
        *pa = va;
        return 0;
    }

    /* The one difference between the two walks: table depth, PTE width, VPN-field
     * width, the PPN mask, and where the ASID sits in satp. */
    int      levels, ptesize, vpnbits;
    uint64_t ppnmask, asid;
    if (scheme == PT_SV32) {
        levels = 2; ptesize = 4; vpnbits = 10;
        ppnmask = 0x3fffffu;            /* 22-bit PPN */
        asid    = (satp >> 22) & 0x1ff;
    } else { /* PT_SV39 */
        levels = 3; ptesize = 8; vpnbits = 9;
        ppnmask = 0xfffffffffffull;     /* 44-bit PPN */
        asid    = (satp >> 44) & 0xffff;
        /* Sv39 VAs must be sign-extended from bit 38; a non-canonical VA faults
         * before the walk even starts. */
        uint64_t sext = (uint64_t)(((int64_t)(va << 25)) >> 25);
        if (sext != va) return pf_cause(acc);
    }

    uint64_t vpn = va >> 12;

    /* TLB lookup, direct-mapped by the low VPN bits (an O(1) probe — the old
     * linear scan of all entries was ~20% of a full-system run, M25 perf). A
     * store may be served from the TLB only when the cached PTE already has its
     * dirty bit set: the walk it skips would find A|D already set and write
     * nothing back, so the hit is equivalent. A store to a clean page still
     * walks, so the dirty bit lands on the real PTE before the write. */
    TlbEntry *e = &cpu->tlb[vpn & (TLB_ENTRIES - 1)];
    if (e->valid && e->vpn == vpn && e->asid == asid &&
        (acc != ACC_STORE || (e->pte_flags & PTE_D))) {
        if (!perm_ok(cpu, e->pte_flags, acc, priv)) return pf_cause(acc);
        *pa = (e->ppn << 12) | (va & 0xfff);
        return 0;
    }

    /* Multi-level walk from the root table at satp.PPN. */
    uint64_t a = (satp & ppnmask) << 12;
    uint64_t pte = 0, pte_addr = 0;
    uint32_t idxmask = (1u << vpnbits) - 1;
    int level;
    for (level = levels - 1; ; level--) {
        uint64_t idx = (va >> (12 + level * vpnbits)) & idxmask;
        pte_addr = a + idx * (uint64_t)ptesize;
        pte = (ptesize == 4) ? mem_read32(cpu->mem, pte_addr)
                             : mem_read64(cpu->mem, pte_addr);
        if (cpu->mem->fault) { cpu->mem->fault = 0; return pf_cause(acc); }
        if (!(pte & PTE_V) || (!(pte & PTE_R) && (pte & PTE_W)))
            return pf_cause(acc);              /* invalid, or W without R */
        if (pte & (PTE_R | PTE_X)) break;      /* a leaf */
        if (level == 0) return pf_cause(acc);  /* no leaf at the last level */
        a = ((pte >> 10) & ppnmask) << 12;     /* descend to the next table */
    }

    if (!perm_ok(cpu, pte, acc, priv)) return pf_cause(acc);

    /* Resolve the physical page number. For a superpage (a leaf above level 0)
     * the low `level` VPN fields come from the VA, and the PTE's matching PPN
     * bits must be zero — a misaligned superpage faults. */
    uint64_t ppn = (pte >> 10) & ppnmask;
    if (level > 0) {
        uint64_t low = ((uint64_t)1 << (level * vpnbits)) - 1;
        if (ppn & low) return pf_cause(acc);   /* misaligned superpage */
        ppn = (ppn & ~low) | ((va >> 12) & low);
    }

    /* Maintain accessed/dirty in hardware: A on any access, D on a store. */
    uint64_t newpte = pte | PTE_A | (acc == ACC_STORE ? PTE_D : 0);
    if (newpte != pte) {
        if (ptesize == 4) mem_write32(cpu->mem, pte_addr, (uint32_t)newpte);
        else              mem_write64(cpu->mem, pte_addr, newpte);
    }

    /* Cache the fresh translation in its direct-mapped slot. A store's fill
     * carries the dirty bit just set, so later stores to the page hit too. */
    e->valid = 1; e->vpn = vpn; e->asid = (uint32_t)asid; e->ppn = ppn;
    e->pte_flags = (uint32_t)(newpte & 0xff);

    *pa = (ppn << 12) | (va & 0xfff);
    return 0;
}

void mmu_flush(CPU *cpu) {
    for (int i = 0; i < TLB_ENTRIES; i++) cpu->tlb[i].valid = 0;
    cpu->tlb_next = 0;
}
