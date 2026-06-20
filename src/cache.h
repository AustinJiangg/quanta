#ifndef QUANTA_CACHE_H
#define QUANTA_CACHE_H

#include <stdint.h>
#include <stdio.h>

/*
 * A set-associative cache model with LRU replacement.
 *
 * This is an *observability layer*, not part of the data path: it watches the
 * addresses of data loads and stores and tallies hits and misses, but the
 * bytes themselves still flow through the flat Memory. Because it never returns
 * data, it cannot change what a program computes — it only reports how that
 * program's memory accesses would behave against a cache of a given geometry.
 *
 * Direct-mapped is simply the associativity-1 case. Writes use a write-allocate
 * policy (a store miss installs the block), which is what matters for hit/miss
 * accounting; write-back vs write-through would only affect memory traffic,
 * which we don't model.
 *
 * Address split (block_size and num_sets are powers of two):
 *
 *     | tag ............ | index (index_bits) | offset (offset_bits) |
 *
 * offset picks a byte within the block, index selects the set, and tag
 * identifies which block currently occupies a line in that set.
 */

typedef struct {
    uint8_t  valid;   /* line holds a block                       */
    uint32_t tag;     /* which block, within its set              */
    uint64_t used;    /* tick of last access, for LRU replacement */
} CacheLine;

/* Tagged so a forward `struct Cache` (e.g. the pointer in CPU) names this exact
 * type rather than a separate incomplete one. */
typedef struct Cache {
    uint32_t block_size;   /* bytes per block (line)            */
    uint32_t num_sets;     /* number of sets                    */
    uint32_t assoc;        /* lines per set (ways); 1 = direct  */
    uint32_t offset_bits;  /* log2(block_size)                  */
    uint32_t index_bits;   /* log2(num_sets)                    */
    CacheLine *lines;      /* num_sets * assoc lines, row-major  */
    uint64_t tick;         /* monotonic clock driving LRU       */
    uint64_t loads, stores, hits, misses; /* statistics         */
} Cache;

/* Initialise a cache of `size_bytes` total, `assoc`-way, with `block_size`-byte
 * blocks. block_size and assoc must be powers of two, and size_bytes must equal
 * num_sets * assoc * block_size for some power-of-two num_sets >= 1. Returns 0
 * on success, -1 on an invalid geometry (a diagnostic is printed to stderr). */
int  cache_init(Cache *c, uint32_t size_bytes, uint32_t assoc,
                uint32_t block_size);

/* Release the line storage. */
void cache_free(Cache *c);

/* Record one data access at `addr` (is_store != 0 for a write). Updates the
 * hit/miss tallies and, on a miss, installs the block (write-allocate),
 * evicting the least-recently-used line in the set. */
void cache_access(Cache *c, uint32_t addr, int is_store);

/* Print a human-readable hit/miss summary to `out`. */
void cache_report(const Cache *c, FILE *out);

#endif /* QUANTA_CACHE_H */
