#ifndef QUANTA_PIPELINE_H
#define QUANTA_PIPELINE_H

#include <stdint.h>
#include <stdio.h>

/*
 * A pipeline *timing* model — an overlay, not a second executor.
 *
 * The interpreter still runs instructions one at a time and produces the real
 * results. This model just watches the dynamic instruction stream as it retires
 * and estimates how many cycles a classic 5-stage pipeline (IF, ID, EX, MEM,
 * WB) would have taken, by adding stall cycles for the hazards such a pipeline
 * cannot hide. It changes nothing the program computes.
 *
 * Modelling assumptions, kept simple and stated up front:
 *
 *   - One instruction issues per cycle; the pipeline costs DEPTH-1 extra cycles
 *     to fill once at the start.
 *   - Full forwarding/bypassing, so most read-after-write data hazards are free.
 *   - The one data hazard that still stalls is load-use: when a load is followed
 *     immediately by an instruction that reads its destination, the value is not
 *     out of MEM in time to forward into the next EX, costing one bubble.
 *   - Control hazards use predict-not-taken: a fall-through branch is free; a
 *     taken conditional branch or a JALR costs 2 (resolved in EX); a JAL costs 1
 *     (its target is known in ID).
 *   - No structural hazards (separate I/D memory) and no cache-miss penalties —
 *     cache behaviour is the separate --cache model.
 */

typedef struct {
    uint64_t instrs;          /* instructions retired                       */
    uint64_t load_use_stalls; /* load-use bubbles (1 cycle each)            */
    uint64_t control_events;  /* taken redirects (taken branch / JAL / JALR) */
    uint64_t control_cycles;  /* penalty cycles charged for those redirects  */
    uint32_t prev_load_rd;    /* destination of the previous instruction if it
                                 was a load with rd != x0, else 0           */
} Pipeline;

/* Zero all counters. */
void pipeline_init(Pipeline *p);

/* Account for one retired instruction `inst`. `redirected` is non-zero when
 * control did not fall through to pc+4 (a taken branch or a jump). */
void pipeline_observe(Pipeline *p, uint32_t inst, int redirected);

/* Print the cycle/CPI estimate and stall breakdown to `out`. */
void pipeline_report(const Pipeline *p, FILE *out);

#endif /* QUANTA_PIPELINE_H */
