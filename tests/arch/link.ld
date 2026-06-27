/*
 * Link script for the RISC-V architectural tests under Quanta.
 *
 * A flat image based at 0x80000000 — Quanta's load base — laid out as the
 * suite expects: the entry stub (.text.init), the tohost page, code, then the
 * data and signature region. The ELF loader sizes guest memory to span this
 * image (plus stack headroom), so the whole thing lives in one flat region.
 *
 * Adapted from the suite's sail-riscv-c example target (BSD-3-Clause).
 */
OUTPUT_ARCH( "riscv" )
ENTRY(rvtest_entry_point)

SECTIONS
{
  . = 0x80000000;
  .text.init : { *(.text.init) }
  . = ALIGN(0x1000);
  .tohost : { *(.tohost) }
  . = ALIGN(0x1000);
  .text : { *(.text) }
  . = ALIGN(0x1000);
  .data : { *(.data) }
  .data.string : { *(.data.string) }
  .bss : { *(.bss) }
  _end = .;
}
