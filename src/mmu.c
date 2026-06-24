#include "mmu.h"
#include "decode.h"

/*
 * Sv32 turns a 32-bit virtual address into a (up to 34-bit, but here 32-bit)
 * physical one through a two-level page table rooted at satp.PPN:
 *
 *   va = | VPN[1] : 10 | VPN[0] : 10 | offset : 12 |
 *
 * The walk indexes the root table by VPN[1] and, unless that entry is already a
 * leaf (a 4 MiB "megapage"), the second-level table by VPN[0]. A leaf PTE
 * supplies the physical page number; its low byte carries valid/permission and
 * the accessed/dirty bits, which we maintain in hardware.
 */

/* satp (Sv32): MODE in bit 31, ASID in [30:22], root PPN in [21:0]. */
#define SATP_SV32 0x80000000u

/* PTE permission/status bits. */
enum {
    PTE_V = 1u << 0, PTE_R = 1u << 1, PTE_W = 1u << 2, PTE_X = 1u << 3,
    PTE_U = 1u << 4, PTE_G = 1u << 5, PTE_A = 1u << 6, PTE_D = 1u << 7
};

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

uint32_t mmu_translate(CPU *cpu, uint32_t va, AccessType acc, uint32_t *pa) {
    uint32_t satp = cpu->csr[CSR_SATP];
    uint32_t priv = eff_priv(cpu, acc);

    /* Machine mode and Bare mode access physical memory directly. */
    if (priv == PRIV_M || !(satp & SATP_SV32)) { *pa = va; return 0; }

    uint32_t asid = (satp >> 22) & 0x1ff;
    uint32_t vpn  = va >> 12;

    /* TLB serves fetches and loads; stores always walk so the dirty bit is set
     * on the real PTE before the write is allowed to land. */
    if (acc != ACC_STORE) {
        for (int i = 0; i < TLB_ENTRIES; i++) {
            TlbEntry *e = &cpu->tlb[i];
            if (e->valid && e->vpn == vpn && e->asid == asid) {
                if (!perm_ok(cpu, e->pte_flags, acc, priv)) return pf_cause(acc);
                *pa = (e->ppn << 12) | (va & 0xfff);
                return 0;
            }
        }
    }

    /* Two-level walk from the root table at satp.PPN. */
    uint32_t a = (satp & 0x3fffffu) << 12;
    uint32_t pte = 0, pte_addr = 0;
    int level;
    for (level = 1; ; level--) {
        uint32_t idx = (va >> (12 + level * 10)) & 0x3ff;
        pte_addr = a + idx * 4;
        pte = mem_read32(cpu->mem, pte_addr);
        if (cpu->mem->fault) { cpu->mem->fault = 0; return pf_cause(acc); }
        if (!(pte & PTE_V) || (!(pte & PTE_R) && (pte & PTE_W)))
            return pf_cause(acc);              /* invalid, or W without R */
        if (pte & (PTE_R | PTE_X)) break;      /* a leaf */
        if (level == 0) return pf_cause(acc);  /* no leaf at the last level */
        a = ((pte >> 10) & 0x3fffffu) << 12;   /* descend to the next table */
    }

    if (!perm_ok(cpu, pte, acc, priv)) return pf_cause(acc);

    uint32_t ppn;
    if (level == 1) {                           /* 4 MiB megapage */
        if ((pte >> 10) & 0x3ff) return pf_cause(acc); /* misaligned superpage */
        ppn = (((pte >> 20) & 0xfff) << 10) | ((va >> 12) & 0x3ff);
    } else {
        ppn = (pte >> 10) & 0x3fffffu;
    }

    /* Maintain accessed/dirty in hardware: A on any access, D on a store. */
    uint32_t newpte = pte | PTE_A | (acc == ACC_STORE ? PTE_D : 0);
    if (newpte != pte) mem_write32(cpu->mem, pte_addr, newpte);

    if (acc != ACC_STORE) { /* cache the fresh translation */
        TlbEntry *e = &cpu->tlb[cpu->tlb_next];
        e->valid = 1; e->vpn = vpn; e->asid = asid; e->ppn = ppn;
        e->pte_flags = newpte & 0xff;
        cpu->tlb_next = (cpu->tlb_next + 1) % TLB_ENTRIES;
    }

    *pa = (ppn << 12) | (va & 0xfff);
    return 0;
}

void mmu_flush(CPU *cpu) {
    for (int i = 0; i < TLB_ENTRIES; i++) cpu->tlb[i].valid = 0;
    cpu->tlb_next = 0;
}
