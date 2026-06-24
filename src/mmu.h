#ifndef QUANTA_MMU_H
#define QUANTA_MMU_H

#include <stdint.h>
#include "cpu.h"
#include "memory.h"

/*
 * Sv32 virtual memory (M12).
 *
 * Once satp enables Sv32 and the hart drops below Machine mode, every
 * instruction fetch and data access goes through a two-level page-table walk
 * that turns a 32-bit virtual address into a physical one. This is the layer
 * that lets each program believe it owns the whole address space: the page
 * tables map its virtual pages onto whatever physical frames the supervisor
 * chose, and an access with no valid mapping — or the wrong permission — raises
 * a precise page-fault exception instead of touching memory.
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
uint32_t mmu_translate(CPU *cpu, uint32_t va, AccessType acc, uint32_t *pa);

/* Invalidate every cached translation. Called on sfence.vma and satp writes. */
void mmu_flush(CPU *cpu);

#endif /* QUANTA_MMU_H */
