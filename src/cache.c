#include "cache.h"

#include <stdlib.h>

/* ------------------------------------------------------------------------
 * A set-associative LRU cache model. See cache.h for the role it plays (a
 * pure observability layer) and the address-split diagram.
 * ------------------------------------------------------------------------ */

static int is_pow2(uint32_t x) {
    return x != 0 && (x & (x - 1)) == 0;
}

/* log2 of a power of two. */
static uint32_t log2u(uint32_t x) {
    uint32_t n = 0;
    while ((1u << n) < x) {
        n++;
    }
    return n;
}

int cache_init(Cache *c, uint32_t size_bytes, uint32_t assoc,
               uint32_t block_size) {
    if (!is_pow2(block_size) || block_size < 4) {
        fprintf(stderr, "cache: block size must be a power of two >= 4 "
                        "(got %u)\n", block_size);
        return -1;
    }
    if (!is_pow2(assoc)) {
        fprintf(stderr, "cache: associativity must be a power of two (got %u)\n",
                assoc);
        return -1;
    }
    uint32_t per_set = assoc * block_size;
    if (size_bytes == 0 || size_bytes % per_set != 0) {
        fprintf(stderr, "cache: size %u must be a multiple of assoc*block "
                        "(%u)\n", size_bytes, per_set);
        return -1;
    }
    uint32_t num_sets = size_bytes / per_set;
    if (!is_pow2(num_sets)) {
        fprintf(stderr, "cache: %u B / (%u-way * %u B) = %u sets is not a power "
                        "of two\n", size_bytes, assoc, block_size, num_sets);
        return -1;
    }

    c->lines = calloc((size_t)num_sets * assoc, sizeof *c->lines);
    if (!c->lines) {
        fprintf(stderr, "cache: cannot allocate %u lines\n", num_sets * assoc);
        return -1;
    }

    c->block_size  = block_size;
    c->num_sets    = num_sets;
    c->assoc       = assoc;
    c->offset_bits = log2u(block_size);
    c->index_bits  = log2u(num_sets);
    c->tick        = 0;
    c->loads = c->stores = c->hits = c->misses = 0;
    return 0;
}

void cache_free(Cache *c) {
    free(c->lines);
    c->lines = NULL;
}

void cache_access(Cache *c, uint32_t addr, int is_store) {
    if (is_store) {
        c->stores++;
    } else {
        c->loads++;
    }
    c->tick++;

    uint32_t index = (addr >> c->offset_bits) & (c->num_sets - 1);
    uint32_t tag   = addr >> (c->offset_bits + c->index_bits);
    CacheLine *set = &c->lines[(size_t)index * c->assoc];

    /* Hit? Scan the ways of this set for a valid line with the same tag. */
    for (uint32_t w = 0; w < c->assoc; w++) {
        if (set[w].valid && set[w].tag == tag) {
            set[w].used = c->tick;
            c->hits++;
            return;
        }
    }

    /* Miss: fill an empty way if there is one, otherwise evict the least-
     * recently-used way in the set, then install this block (write-allocate). */
    c->misses++;
    uint32_t victim = 0;
    for (uint32_t w = 0; w < c->assoc; w++) {
        if (!set[w].valid) {
            victim = w;
            break;
        }
        if (set[w].used < set[victim].used) {
            victim = w;
        }
    }
    set[victim].valid = 1;
    set[victim].tag   = tag;
    set[victim].used  = c->tick;
}

void cache_report(const Cache *c, FILE *out) {
    uint64_t acc = c->hits + c->misses;
    double miss_rate = acc ? 100.0 * (double)c->misses / (double)acc : 0.0;
    uint32_t total = c->num_sets * c->assoc * c->block_size;

    fprintf(out, "cache: %u B, %u-way, %u B blocks (%u sets)\n",
            total, c->assoc, c->block_size, c->num_sets);
    fprintf(out, "  accesses %llu  hits %llu  misses %llu  miss-rate %.2f%%\n",
            (unsigned long long)acc, (unsigned long long)c->hits,
            (unsigned long long)c->misses, miss_rate);
    fprintf(out, "  loads %llu  stores %llu\n",
            (unsigned long long)c->loads, (unsigned long long)c->stores);
}
