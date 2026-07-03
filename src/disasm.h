#ifndef QUANTA_DISASM_H
#define QUANTA_DISASM_H

#include <stdint.h>
#include <stddef.h>

/*
 * Disassemble one RV32/RV64 instruction word into `buf` (truncated to `buflen`).
 *
 * `pc` is the address the instruction lives at; it is needed to render the
 * absolute target of PC-relative control transfers (branches and JAL), which
 * is what objdump prints. `xlen` (32 or 64) selects the width-dependent reading:
 * the 6-bit RV64 shift amount, the RV64C compressed forms, and the *W / LD/SD /
 * doubleword-atomic mnemonics. The output uses ABI register names and the common
 * pseudo-instructions (nop/mv/li/j/jr/ret/beqz/...) so it lines up with
 * `objdump -d` on ordinary code. Unknown encodings render as ".word 0x...".
 */
void disasm(uint64_t pc, uint32_t inst, int xlen, char *buf, size_t buflen);

#endif /* QUANTA_DISASM_H */
