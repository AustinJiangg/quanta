#ifndef QUANTA_DECODECACHE_H
#define QUANTA_DECODECACHE_H

#include <stdint.h>

/*
 * A decoded-instruction cache (M25a) — the first performance step from a pure
 * switch-dispatched interpreter toward dynamic translation.
 *
 * cpu_step() otherwise redoes, on every single execution of an instruction, the
 * work that only depends on the instruction word: read the halfword(s) from
 * memory, decide the length from the low two bits, and (for a compressed one)
 * expand the 16-bit form to its 32-bit equivalent. This cache memoises exactly
 * that. Each slot caches, keyed by the *physical* address of an instruction's
 * low halfword, the expanded 32-bit instruction and its true length (2 or 4).
 * On a hit cpu_step() reuses them and skips the fetch reads and the expansion;
 * the opcode dispatch and the exec_* semantics that follow are unchanged. So a
 * run with the cache is bit-identical to the plain interpreter (which is what
 * a hart with cpu->dcache == NULL is) — it is a pure speed overlay, the same
 * contract the cache and pipeline models hold.
 *
 * Why key by physical address. The bytes at a physical address do not move when
 * the guest switches address space, so a kernel's hot code stays decoded across
 * satp writes and sfence.vma — those must NOT flush the cache (they are far too
 * frequent, and PA keying makes flushing on them unnecessary). The one thing
 * that changes which instruction lives at a physical address is a write to that
 * memory; per the RISC-V Zifencei contract a hart must execute FENCE.I before it
 * may fetch instructions its own stores produced, and a store from another agent
 * (a sibling hart, or DMA) likewise requires a FENCE.I on the fetching hart. So
 * cpu_step() flushes the cache on FENCE.I — an O(1) generation bump — which is
 * precisely, and only, where the architecture says a modified instruction must
 * become visible. (Before M25a FENCE.I was a no-op "no modelled icache to
 * flush"; now there is one.)
 *
 * The cache is per-hart: FENCE.I is hart-local, so one hart's flush leaves the
 * others' decoded entries intact.
 */

/* One memoised instruction. `gen` tags the generation the slot was filled under
 * so a flush (a single ++ of the cache generation) retires every slot at once. */
typedef struct {
    uint64_t gen;   /* generation this slot was filled under; live iff == cache gen */
    uint64_t pa;    /* physical address of the low halfword — the tag */
    uint32_t inst;  /* the expanded 32-bit instruction word */
    uint32_t ilen;  /* instruction length in bytes: 2 or 4 */
} DecodedInsn;

/* 2^16 direct-mapped slots. Instructions are at least 2-byte aligned, so the
 * index drops the always-zero low bit (pa >> 1). One slot is 24 bytes, so the
 * table is ~1.5 MiB — which is why it is heap-allocated, never inlined in CPU
 * (whose value copy backs the snapshot). */
#define DC_BITS  16
#define DC_SIZE  (1u << DC_BITS)
#define DC_INDEX(pa) ((uint32_t)((pa) >> 1) & (DC_SIZE - 1))

typedef struct DecodeCache {
    uint64_t    gen;             /* current generation; ++ invalidates every slot */
    DecodedInsn slots[DC_SIZE];
} DecodeCache;

/* Allocate a zeroed, empty cache (NULL on OOM). The generation starts at 1 so a
 * calloc'd slot (gen 0) reads as stale and a physical address of 0 can never
 * false-hit an empty table. */
DecodeCache *dcache_new(void);

/* Release a cache. Safe on NULL. */
void dcache_free(DecodeCache *dc);

/* Invalidate every slot in O(1) — the FENCE.I flush, and used on machine restore
 * (the restored RAM may hold different code). Safe on NULL. */
static inline void dcache_flush(DecodeCache *dc) {
    if (dc) dc->gen++;
}

#endif /* QUANTA_DECODECACHE_H */
