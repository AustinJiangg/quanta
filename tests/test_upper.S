/* RV32I conformance: LUI / AUIPC (the U-type upper-immediate instructions). */
#include "test_framework.h"
    TEST_START

    /* LUI puts the immediate in bits [31:12] and zeros [11:0]. */
    lui t0, 0x12345
    CHECK 1, t0, 0x12345000
    lui t0, 0xfffff
    CHECK 2, t0, 0xfffff000

    /* AUIPC adds (imm << 12) to the current PC. Two adjacent `auipc rd,0`
     * capture consecutive PCs, so they differ by one instruction (4 bytes). */
    auipc t0, 0
    auipc t1, 0
    sub   t2, t1, t0
    CHECK 3, t2, 4

    /* With a nonzero immediate, AUIPC adds imm<<12 on top of the PC delta. */
    auipc t0, 0
    auipc t1, 1              /* (pc0 + 4) + 0x1000 */
    sub   t2, t1, t0
    CHECK 4, t2, 0x1004

    TEST_END
