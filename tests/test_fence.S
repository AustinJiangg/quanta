/* RV32I conformance: FENCE.
 *
 * FENCE orders memory accesses across harts. On a single hart executing in
 * program order it has no observable effect, so it must run as a no-op rather
 * than trap. This test fails (the program halts before its check) if FENCE
 * isn't handled.
 *
 * (FENCE.I, the instruction-stream fence, is the separate Zifencei extension,
 * not base RV32I, so it isn't exercised here; the emulator treats the whole
 * MISC-MEM opcode as a no-op, which covers it too.) */
#include "test_framework.h"
    TEST_START

    li   t0, 1
    fence                    /* must be a no-op, not a halt */
    addi t0, t0, 1
    CHECK 1, t0, 2

    TEST_END
