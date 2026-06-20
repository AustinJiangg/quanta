#include "pipeline.h"
#include "decode.h"

/* ------------------------------------------------------------------------
 * The 5-stage timing model. See pipeline.h for the assumptions; the work here
 * is just classifying each instruction and charging stall cycles for the
 * hazards a forwarding pipeline still cannot hide.
 * ------------------------------------------------------------------------ */

enum { PIPELINE_DEPTH = 5 }; /* IF ID EX MEM WB -> DEPTH-1 fill cycles */

/* Does `inst` read general-purpose register `r` (r != 0) as a source operand?
 * Which source fields an instruction reads depends only on its opcode. */
static int reads_reg(uint32_t inst, uint32_t r) {
    if (r == 0) {
        return 0; /* x0 is never a real dependency */
    }
    uint32_t op = opcode(inst);
    int uses_rs1 = (op == OP_IMM || op == OP_REG || op == OP_LOAD ||
                    op == OP_STORE || op == OP_BRANCH || op == OP_JALR);
    int uses_rs2 = (op == OP_REG || op == OP_STORE || op == OP_BRANCH);
    if (uses_rs1 && rs1(inst) == r) {
        return 1;
    }
    if (uses_rs2 && rs2(inst) == r) {
        return 1;
    }
    return 0;
}

void pipeline_init(Pipeline *p) {
    p->instrs          = 0;
    p->load_use_stalls = 0;
    p->control_events  = 0;
    p->control_cycles  = 0;
    p->prev_load_rd    = 0;
}

void pipeline_observe(Pipeline *p, uint32_t inst, int redirected) {
    /* Data hazard: if the previous instruction was a load and this one reads
     * its result, forwarding can't deliver it in time -> one bubble. */
    if (p->prev_load_rd && reads_reg(inst, p->prev_load_rd)) {
        p->load_use_stalls++;
    }

    /* Control hazard: a taken redirect flushed the wrong-path fetches behind it.
     * JAL's target is known in ID (1 cycle); a taken conditional branch or a
     * JALR is resolved in EX (2 cycles). */
    if (redirected) {
        uint32_t pen = (opcode(inst) == OP_JAL) ? 1u : 2u;
        p->control_events++;
        p->control_cycles += pen;
    }

    p->instrs++;
    p->prev_load_rd = (opcode(inst) == OP_LOAD) ? rd(inst) : 0;
}

void pipeline_report(const Pipeline *p, FILE *out) {
    uint64_t fill   = (p->instrs > 0) ? (uint64_t)(PIPELINE_DEPTH - 1) : 0;
    uint64_t cycles = p->instrs + fill + p->load_use_stalls + p->control_cycles;
    double   cpi    = p->instrs ? (double)cycles / (double)p->instrs : 0.0;

    fprintf(out, "pipeline (5-stage, full forwarding, predict-not-taken):\n");
    fprintf(out, "  instructions %llu  cycles %llu  CPI %.2f\n",
            (unsigned long long)p->instrs, (unsigned long long)cycles, cpi);
    fprintf(out, "  stalls: load-use %llu (%llu cyc)  control %llu (%llu cyc)  "
                 "fill %llu cyc\n",
            (unsigned long long)p->load_use_stalls,
            (unsigned long long)p->load_use_stalls,
            (unsigned long long)p->control_events,
            (unsigned long long)p->control_cycles,
            (unsigned long long)fill);
}
