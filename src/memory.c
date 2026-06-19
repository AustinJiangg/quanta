#include "memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fatal_oob(uint32_t addr) {
    fprintf(stderr, "memory access out of range: 0x%08x\n", addr);
    exit(1);
}

/* Translate a guest address to an index into `data`, aborting if it (or the
 * `width` bytes starting there) falls outside the mapped region. */
static uint32_t translate(const Memory *mem, uint32_t addr, uint32_t width) {
    if (addr < mem->base || addr + width > mem->base + mem->size) {
        fatal_oob(addr);
    }
    return addr - mem->base;
}

int mem_init(Memory *mem, uint32_t base, uint32_t size) {
    mem->data = calloc(size, 1);
    if (!mem->data) {
        return -1;
    }
    mem->base = base;
    mem->size = size;
    return 0;
}

void mem_free(Memory *mem) {
    free(mem->data);
    mem->data = NULL;
    mem->size = 0;
}

/* Little-endian: byte 0 is least significant. */
uint32_t mem_read32(const Memory *mem, uint32_t addr) {
    uint32_t i = translate(mem, addr, 4);
    return (uint32_t)mem->data[i]
         | (uint32_t)mem->data[i + 1] << 8
         | (uint32_t)mem->data[i + 2] << 16
         | (uint32_t)mem->data[i + 3] << 24;
}

uint16_t mem_read16(const Memory *mem, uint32_t addr) {
    uint32_t i = translate(mem, addr, 2);
    return (uint16_t)(mem->data[i] | mem->data[i + 1] << 8);
}

uint8_t mem_read8(const Memory *mem, uint32_t addr) {
    uint32_t i = translate(mem, addr, 1);
    return mem->data[i];
}

void mem_write32(Memory *mem, uint32_t addr, uint32_t value) {
    uint32_t i = translate(mem, addr, 4);
    mem->data[i]     = (uint8_t)(value);
    mem->data[i + 1] = (uint8_t)(value >> 8);
    mem->data[i + 2] = (uint8_t)(value >> 16);
    mem->data[i + 3] = (uint8_t)(value >> 24);
}

void mem_write16(Memory *mem, uint32_t addr, uint16_t value) {
    uint32_t i = translate(mem, addr, 2);
    mem->data[i]     = (uint8_t)(value);
    mem->data[i + 1] = (uint8_t)(value >> 8);
}

void mem_write8(Memory *mem, uint32_t addr, uint8_t value) {
    uint32_t i = translate(mem, addr, 1);
    mem->data[i] = value;
}

void mem_load(Memory *mem, uint32_t addr, const uint8_t *src, size_t len) {
    uint32_t i = translate(mem, addr, (uint32_t)len);
    memcpy(mem->data + i, src, len);
}
