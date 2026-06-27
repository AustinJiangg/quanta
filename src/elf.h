#ifndef QUANTA_ELF_H
#define QUANTA_ELF_H

#include <stdint.h>
#include "memory.h"

/*
 * Minimal ELF32 loader for RV32I executables.
 *
 * An ELF file is two things at once: a description of how to build a memory
 * image, and the bytes to build it from. The loader reads the ELF header to
 * find the program-header table, walks that table, and copies every PT_LOAD
 * segment from its file offset to its virtual address in guest memory. The
 * entry point recorded in the header becomes the initial PC.
 *
 * We parse only what a static, pre-linked RV32I executable needs: the ELF
 * header and the program headers. Sections, symbols, and relocations are
 * ignored — they matter to linkers, not to running an already-linked image.
 */

/* Load the ELF32 executable at `path`, reporting its entry point in `*entry`.
 *
 * On success `*mem` is allocated and initialised to span the program's load
 * image — from the lowest PT_LOAD virtual address to the highest — every
 * PT_LOAD segment is copied to its virtual address, and the caller owns `*mem`
 * and must mem_free() it. `*mem` need not be initialised on entry; the loader
 * discovers the load address from the ELF rather than assuming a fixed base.
 *
 * Returns 0 on success, or -1 on any error (a diagnostic is printed to stderr).
 * On failure `*mem` is left unallocated and `*entry` is left unset. */
int elf_load(const char *path, Memory *mem, uint32_t *entry);

/* Look up a symbol by name in the ELF at `path`, writing its address (st_value)
 * to `*out`. Returns 0 if found, -1 otherwise (parse error, a stripped object
 * with no symbol table, or no such symbol). Unlike elf_load this reads the
 * file's section and symbol tables — which running an image does not need — so
 * it is a separate, quiet pass that does not require the image to be loaded.
 * The --signature dump uses it to locate the begin_signature/end_signature
 * markers that the RISC-V architectural tests bracket their result region with. */
int elf_symbol(const char *path, const char *name, uint32_t *out);

#endif /* QUANTA_ELF_H */
