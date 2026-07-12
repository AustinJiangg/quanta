/*
 * Decoded-instruction cache allocation (M25a). The lookup and fill live inline
 * in cpu_step() for speed; this file owns only the table's lifecycle. See
 * decodecache.h for the design and the FENCE.I invalidation contract.
 */

#include <stdlib.h>
#include "decodecache.h"

DecodeCache *dcache_new(void) {
    DecodeCache *dc = calloc(1, sizeof(*dc)); /* zeroed slots => all stale */
    if (dc) dc->gen = 1;                      /* so a zeroed (gen 0) slot never hits */
    return dc;
}

void dcache_free(DecodeCache *dc) {
    free(dc);
}
