#ifndef QUANTA_MEMORY_H
#define QUANTA_MEMORY_H

#include <stdint.h>
#include <stddef.h>

/*
 * A flat, byte-addressable memory.
 *
 * Real machines have a rich memory hierarchy (caches, TLBs, MMIO regions).
 * For the MVP we model the simplest possible thing: one contiguous array of
 * bytes starting at a fixed base address. RISC-V is little-endian, so
 * multi-byte loads and stores assemble/disassemble bytes low-first.
 */
typedef struct {
    uint8_t *data;  /* backing storage */
    uint32_t base;  /* address that maps to data[0] */
    uint32_t size;  /* number of bytes */
} Memory;

/* Allocate `size` bytes of zeroed memory mapped at `base`.
 * Returns 0 on success, -1 on allocation failure. */
int mem_init(Memory *mem, uint32_t base, uint32_t size);

/* Free the backing storage. */
void mem_free(Memory *mem);

/* Aligned-agnostic little-endian accessors. Out-of-range access is treated
 * as a fatal error (the MVP has no fault handling yet). */
uint32_t mem_read32(const Memory *mem, uint32_t addr);
uint16_t mem_read16(const Memory *mem, uint32_t addr);
uint8_t  mem_read8 (const Memory *mem, uint32_t addr);

void mem_write32(Memory *mem, uint32_t addr, uint32_t value);
void mem_write16(Memory *mem, uint32_t addr, uint16_t value);
void mem_write8 (Memory *mem, uint32_t addr, uint8_t  value);

/* Copy `len` bytes from `src` into memory at `addr` (used to load a program
 * image). */
void mem_load(Memory *mem, uint32_t addr, const uint8_t *src, size_t len);

#endif /* QUANTA_MEMORY_H */
