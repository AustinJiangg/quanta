#ifndef QUANTA_MEMORY_H
#define QUANTA_MEMORY_H

#include <stdint.h>
#include <stddef.h>

/*
 * A flat, byte-addressable memory with optional memory-mapped I/O.
 *
 * The bulk of the address space is one contiguous array of bytes starting at a
 * fixed base address (RISC-V is little-endian, so multi-byte loads and stores
 * assemble/disassemble bytes low-first). When a platform is attached (M13), a
 * few physical-address windows are carved out for device registers instead:
 * accesses to those are dispatched to the device models rather than RAM. The
 * dispatch lives here so the CPU's load/store paths stay oblivious to it.
 */
struct Platform; /* device models (device.h); attached for MMIO, else NULL */

typedef struct {
    uint8_t *data;          /* backing storage */
    uint32_t base;          /* address that maps to data[0] */
    uint32_t size;          /* number of bytes */
    int      fault;         /* set by an out-of-range access; cleared each step */
    uint32_t fault_addr;    /* the offending address when `fault` is set */
    struct Platform *plat;  /* MMIO device platform, or NULL for plain RAM */
} Memory;

/* Allocate `size` bytes of zeroed memory mapped at `base`.
 * Returns 0 on success, -1 on allocation failure. */
int mem_init(Memory *mem, uint32_t base, uint32_t size);

/* Free the backing storage. */
void mem_free(Memory *mem);

/* Alignment-agnostic little-endian accessors. An out-of-range access sets
 * `mem->fault` (and `fault_addr`), reads back as 0, and drops a write — rather
 * than terminating the host. The CPU clears the flag each step and turns a set
 * flag into a clean halt, so the reads are no longer `const`. */
uint32_t mem_read32(Memory *mem, uint32_t addr);
uint16_t mem_read16(Memory *mem, uint32_t addr);
uint8_t  mem_read8 (Memory *mem, uint32_t addr);

void mem_write32(Memory *mem, uint32_t addr, uint32_t value);
void mem_write16(Memory *mem, uint32_t addr, uint16_t value);
void mem_write8 (Memory *mem, uint32_t addr, uint8_t  value);

/* Copy `len` bytes from `src` into memory at `addr` (used to load a program
 * image). Returns 0 on success, -1 if the destination range is out of bounds. */
int mem_load(Memory *mem, uint32_t addr, const uint8_t *src, size_t len);

#endif /* QUANTA_MEMORY_H */
