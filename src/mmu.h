#ifndef QUANTA_MMU_H
#define QUANTA_MMU_H

#include <stdint.h>
#include "cpu.h"
#include "memory.h"

/*
 * Virtual memory: Sv32 on RV32 (M12), Sv39 on RV64 (M18).
 *
 * Once satp enables paging and the hart drops below Machine mode, every
 * instruction fetch and data access goes through a multi-level page-table walk
 * that turns a virtual address into a physical one — a two-level walk for Sv32,
 * a three-level walk for Sv39. This is the layer that lets each program believe
 * it owns the whole address space: the page tables map its virtual pages onto
 * whatever physical frames the supervisor chose, and an access with no valid
 * mapping — or the wrong permission — raises a precise page-fault exception
 * instead of touching memory.
 *
 * The walk is the slow path; a small TLB caches recent translations. Because a
 * stale entry would hide page-table edits, the TLB is flushed by sfence.vma and
 * by any write to satp.
 */

/* Translate `va` for an access of type `acc`, yielding the physical address in
 * *pa. Returns 0 on success, or the page-fault cause (CAUSE_*_PAGE_FAULT) when
 * the mapping is missing or the permission check fails — the caller raises that
 * as a trap with `va` as the trap value. In Machine mode, or when satp selects
 * Bare mode, translation is the identity and this always succeeds. */
uint32_t mmu_translate(CPU *cpu, uint64_t va, AccessType acc, uint64_t *pa);

/* Invalidate every cached translation. Called on sfence.vma and satp writes. */
void mmu_flush(CPU *cpu);

/* Is `val` a satp value Quanta can honour, i.e. does its MODE select a paging
 * scheme we model (Bare, Sv32 on RV32, Sv39 on RV64)? satp is WARL, so a write
 * that selects an unsupported scheme (e.g. Sv48/Sv57) must be dropped rather
 * than stored — which is how a guest probing for the widest supported mode
 * (Linux tries Sv57, then Sv48, then Sv39) sees the unsupported ones not stick.
 * csr_write consults this before committing a satp write. */
int mmu_satp_supported(const CPU *cpu, uint64_t val);

#endif /* QUANTA_MMU_H */
