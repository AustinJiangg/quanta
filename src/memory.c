#include "memory.h"
#include "device.h"

#include <stdlib.h>
#include <string.h>

/* Translate a guest address to an index into `data`. If the access (the
 * address, or the `width` bytes starting there) falls outside the mapped
 * region, record a fault in `mem` and return 0 instead of terminating the
 * host: the caller reads back zero or drops the write, and the CPU turns the
 * flag into a clean halt. The comparisons are arranged to avoid address-space
 * wrap for regions sitting near the top of the address space. */
static int translate(Memory *mem, uint64_t addr, uint64_t width, uint64_t *out) {
    if (addr < mem->base || width > mem->size ||
        addr - mem->base > mem->size - width) {
        mem->fault      = 1;
        mem->fault_addr = addr;
        return 0;
    }
    *out = addr - mem->base;
    return 1;
}

int mem_init(Memory *mem, uint64_t base, uint64_t size) {
    mem->data = calloc(size, 1);
    if (!mem->data) {
        return -1;
    }
    mem->base       = base;
    mem->size       = size;
    mem->fault      = 0;
    mem->fault_addr = 0;
    mem->plat       = NULL; /* no devices until a platform is attached */
    return 0;
}

void mem_free(Memory *mem) {
    free(mem->data);
    mem->data = NULL;
    mem->size = 0;
}

/* Little-endian: byte 0 is least significant. A faulting read returns 0. An
 * address in a device window is dispatched to the platform, not RAM. A
 * doubleword MMIO access decomposes into two 32-bit device reads (the CLINT's
 * 64-bit mtime/mtimecmp are register pairs), so an RV64 `ld` of it works. */
uint64_t mem_read64(Memory *mem, uint64_t addr) {
    uint64_t i;
    if (mem->plat && plat_contains(addr))
        return (uint64_t)plat_read(mem->plat, addr, 4)
             | (uint64_t)plat_read(mem->plat, addr + 4, 4) << 32;
    if (!translate(mem, addr, 8, &i)) return 0;
    return (uint64_t)mem->data[i]
         | (uint64_t)mem->data[i + 1] << 8
         | (uint64_t)mem->data[i + 2] << 16
         | (uint64_t)mem->data[i + 3] << 24
         | (uint64_t)mem->data[i + 4] << 32
         | (uint64_t)mem->data[i + 5] << 40
         | (uint64_t)mem->data[i + 6] << 48
         | (uint64_t)mem->data[i + 7] << 56;
}

uint32_t mem_read32(Memory *mem, uint64_t addr) {
    uint64_t i;
    if (mem->plat && plat_contains(addr)) return plat_read(mem->plat, addr, 4);
    if (!translate(mem, addr, 4, &i)) return 0;
    return (uint32_t)mem->data[i]
         | (uint32_t)mem->data[i + 1] << 8
         | (uint32_t)mem->data[i + 2] << 16
         | (uint32_t)mem->data[i + 3] << 24;
}

uint16_t mem_read16(Memory *mem, uint64_t addr) {
    uint64_t i;
    if (mem->plat && plat_contains(addr)) return (uint16_t)plat_read(mem->plat, addr, 2);
    if (!translate(mem, addr, 2, &i)) return 0;
    return (uint16_t)(mem->data[i] | mem->data[i + 1] << 8);
}

uint8_t mem_read8(Memory *mem, uint64_t addr) {
    uint64_t i;
    if (mem->plat && plat_contains(addr)) return (uint8_t)plat_read(mem->plat, addr, 1);
    if (!translate(mem, addr, 1, &i)) return 0;
    return mem->data[i];
}

/* A faulting store is dropped; a store to a device window goes to the platform.
 * A doubleword MMIO store splits into two 32-bit device writes (see mem_read64). */
void mem_write64(Memory *mem, uint64_t addr, uint64_t value) {
    uint64_t i;
    if (mem->plat && plat_contains(addr)) {
        plat_write(mem->plat, addr,     4, (uint32_t)value);
        plat_write(mem->plat, addr + 4, 4, (uint32_t)(value >> 32));
        return;
    }
    if (!translate(mem, addr, 8, &i)) return;
    for (int b = 0; b < 8; b++) mem->data[i + (uint64_t)b] = (uint8_t)(value >> (8 * b));
}

void mem_write32(Memory *mem, uint64_t addr, uint32_t value) {
    uint64_t i;
    if (mem->plat && plat_contains(addr)) { plat_write(mem->plat, addr, 4, value); return; }
    if (!translate(mem, addr, 4, &i)) return;
    mem->data[i]     = (uint8_t)(value);
    mem->data[i + 1] = (uint8_t)(value >> 8);
    mem->data[i + 2] = (uint8_t)(value >> 16);
    mem->data[i + 3] = (uint8_t)(value >> 24);
}

void mem_write16(Memory *mem, uint64_t addr, uint16_t value) {
    uint64_t i;
    if (mem->plat && plat_contains(addr)) { plat_write(mem->plat, addr, 2, value); return; }
    if (!translate(mem, addr, 2, &i)) return;
    mem->data[i]     = (uint8_t)(value);
    mem->data[i + 1] = (uint8_t)(value >> 8);
}

void mem_write8(Memory *mem, uint64_t addr, uint8_t value) {
    uint64_t i;
    if (mem->plat && plat_contains(addr)) { plat_write(mem->plat, addr, 1, value); return; }
    if (!translate(mem, addr, 1, &i)) return;
    mem->data[i] = value;
}

int mem_load(Memory *mem, uint64_t addr, const uint8_t *src, size_t len) {
    uint64_t i;
    if (!translate(mem, addr, (uint64_t)len, &i)) return -1;
    memcpy(mem->data + i, src, len);
    return 0;
}
