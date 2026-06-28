/*
 * Link script for the M16 teaching kernel.
 *
 * A flat image based at Quanta's load address (0x80000000), the same base the
 * conformance tests use. The ELF loader copies the PT_LOAD range here and, with
 * --memory, leaves spare RAM above it that the kernel hands out as physical
 * pages. _end is page-aligned so the kernel's page allocator can start there.
 *
 * The user payload (user.S, section .user) is a position-independent blob the
 * kernel copies into a fresh page and maps at a low virtual address; it is
 * bracketed by __user_start/__user_end and kept out of .text so the kernel
 * never executes it in place.
 */
OUTPUT_ARCH("riscv")
ENTRY(_start)

SECTIONS
{
  . = 0x80000000;

  .text : {
    *(.text.boot)            /* _start sits at the entry address */
    *(.text .text.*)
  }

  . = ALIGN(8);
  .rodata : { *(.rodata .rodata.*) }

  . = ALIGN(8);
  .data : { *(.data .data.* .sdata .sdata.*) }

  . = ALIGN(16);
  .bss : { *(.bss .bss.* .sbss .sbss.* COMMON) }

  . = ALIGN(16);
  .user : {
    __user_start = .;
    *(.user .user.*)
    __user_end = .;
  }

  . = ALIGN(4096);
  _end = .;
}
