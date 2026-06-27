/*
 * Quanta target definitions for the RISC-V architectural test suite
 * (riscv-non-isa/riscv-arch-test, old-framework-2.x).
 *
 * Each test #includes this header (the "model"/DUT glue) and the framework's
 * arch_test.h. The framework writes its results into a region the test brackets
 * with the begin_signature/end_signature symbols defined below; afterwards the
 * simulator dumps that region and the harness diffs it against the suite's
 * committed reference signature. Quanta dumps it with `--signature=FILE`, which
 * resolves those two symbols from the ELF.
 *
 * Halting: rather than the framework's tohost/HTIF convention, Quanta stops via
 * its built-in SEE — the exit(0) syscall (a7=93). The test has already filled
 * the signature region by the time RVMODEL_HALT runs, so a clean exit is all
 * that is needed, and it needs no extra device model. The I/M/Zifencei families
 * never install a trap handler that survives to the halt, so the ecall reaches
 * the SEE and exits cleanly.
 *
 * The console (RVMODEL_IO_*) and interrupt (RVMODEL_*_INT) hooks are no-ops:
 * conformance is decided by the signature, not by printed output, and Quanta has
 * no interrupt sources yet (devices are a later milestone). Adapted from the
 * suite's sail-riscv-c example target (BSD-3-Clause), trimmed to Quanta's needs.
 */
#ifndef _COMPLIANCE_MODEL_H
#define _COMPLIANCE_MODEL_H

#define RVMODEL_DATA_SECTION \
        .pushsection .tohost,"aw",@progbits;                            \
        .align 8; .global tohost; tohost: .dword 0;                     \
        .align 8; .global fromhost; fromhost: .dword 0;                 \
        .popsection;                                                    \
        .align 8; .global begin_regstate; begin_regstate:               \
        .word 128;                                                      \
        .align 8; .global end_regstate; end_regstate:                   \
        .word 4;

// Halt via Quanta's built-in SEE: exit(0). The signature region is already
// populated by the test body, so a clean exit is all that is required.
#define RVMODEL_HALT \
  li a7, 93; \
  li a0, 0;  \
  ecall;

#define RVMODEL_BOOT

// Start of the signature region (16-byte aligned).
#define RVMODEL_DATA_BEGIN \
  .align 4; .global begin_signature; begin_signature:

// End of the signature region, followed by the tohost/regstate data the
// framework and link script expect to exist.
#define RVMODEL_DATA_END \
  .align 4; .global end_signature; end_signature: \
  RVMODEL_DATA_SECTION

// Console output is unused — the signature decides pass/fail.
#define RVMODEL_IO_INIT
#define RVMODEL_IO_WRITE_STR(_R, _STR)
#define RVMODEL_IO_CHECK()
#define RVMODEL_IO_ASSERT_GPR_EQ(_S, _R, _I)
#define RVMODEL_IO_ASSERT_SFPR_EQ(_F, _R, _I)
#define RVMODEL_IO_ASSERT_DFPR_EQ(_D, _R, _I)

// No interrupt sources are modelled yet.
#define RVMODEL_SET_MSW_INT
#define RVMODEL_CLEAR_MSW_INT
#define RVMODEL_CLEAR_MTIMER_INT
#define RVMODEL_CLEAR_MEXT_INT

#endif // _COMPLIANCE_MODEL_H
