#include "quanta.h"

#include "cpu.h"
#include "memory.h"
#include "elf.h"
#include "cache.h"
#include "device.h"
#include "dtb.h"
#include "decode.h"

#include <stdlib.h>
#include <string.h>

/* The library version string; the QUANTA_VERSION_* macros in quanta.h are the
 * single source of truth (kept in sync with the CHANGELOG and the git tag). */
const char *quanta_version(void) {
    return QUANTA_VERSION_STRING;
}

/* A generous runaway guard for quanta_run(max_steps == 0): high enough for real
 * workloads (array loops, deep call chains), low enough that a program that
 * never halts still stops in about a second instead of hanging. */
#define QUANTA_DEFAULT_MAX_STEPS (100ULL * 1000 * 1000)

/*
 * The handle owns every piece of the machine. CPU holds a borrowed pointer to
 * Memory (and, when enabled, the Cache), so the three live together here and
 * are wired up by the loaders.
 */
struct Quanta {
    CPU      cpu;
    Memory   mem;
    Cache    cache;
    Platform plat;    /* MMIO devices: CLINT timer/IPI, PLIC, 16550 UART (M13) */
    uint32_t dtb_addr;  /* where the boot device tree was placed (0 if none, M14) */
    int      cache_on;  /* a cache has been attached */
    int      loaded;    /* memory is initialised and PC/sp are set */
};

/* Map the internal halt reason to the public enum. */
static QuantaHalt map_halt(HaltReason r) {
    switch (r) {
        case HALT_NONE:            return QUANTA_RUN;
        case HALT_EXIT:            return QUANTA_HALT_EXIT;
        case HALT_EBREAK:          return QUANTA_HALT_EBREAK;
        case HALT_ILLEGAL_INSN:    return QUANTA_HALT_ILLEGAL_INSN;
        case HALT_UNIMP_SYSTEM:    return QUANTA_HALT_UNIMP_SYSTEM;
        case HALT_UNKNOWN_SYSCALL: return QUANTA_HALT_UNKNOWN_SYSCALL;
        case HALT_MEM_FAULT:       return QUANTA_HALT_MEM_FAULT;
    }
    return QUANTA_RUN;
}

/* Bounds check shared by the peek/poke helpers, written to avoid 32-bit wrap.
 * Returns the host offset for `addr` (valid only when it returns QUANTA_OK). */
static QuantaStatus in_range(const Memory *mem, uint32_t addr, size_t len,
                            uint32_t *off) {
    if (addr < mem->base || len > mem->size ||
        addr - mem->base > mem->size - (uint32_t)len) {
        return QUANTA_ERR_RANGE;
    }
    *off = addr - mem->base;
    return QUANTA_OK;
}

Quanta *quanta_create(void) {
    return calloc(1, sizeof(Quanta)); /* NULL on OOM */
}

void quanta_destroy(Quanta *q) {
    if (!q) return;
    if (q->cache_on) cache_free(&q->cache);
    mem_free(&q->mem);
    free(q);
}

/* Shared tail of the loaders: attach the CPU to memory at `entry`, re-attach the
 * cache if one is enabled, and set sp to the top of the region (16-byte aligned
 * per the RISC-V ABI) so the program can call functions and spill locals. */
static void start_at(Quanta *q, uint32_t entry) {
    cpu_init(&q->cpu, &q->mem, entry);
    if (q->cache_on) q->cpu.cache = &q->cache;
    plat_init(&q->plat);      /* reset the MMIO devices */
    q->mem.plat = &q->plat;   /* attach them so MMIO is dispatched and timers tick */
    q->dtb_addr = 0;          /* set only by the boot handoff below (ELF path) */
    reg_write(&q->cpu, 2, (q->mem.base + q->mem.size) & ~(uint32_t)0xf);
    q->loaded = 1;
}

/* The RISC-V boot handoff (M14): describe this machine in a flattened device
 * tree, drop it at the top of guest memory, and enter the guest the way firmware
 * does — a0 = boot hart id, a1 = the DTB's physical address — with sp moved just
 * below the tree so the stack never grows into it. So a kernel can discover its
 * RAM and devices instead of assuming a fixed layout. The tree is tiny (well
 * under a page) and sits in the loader's stack headroom; if it somehow does not
 * fit, we leave start_at's plain entry state untouched (a0/a1 = 0). */
static void setup_boot(Quanta *q) {
    DtbConfig cfg = {
        .mem_base   = q->mem.base,   .mem_size  = q->mem.size,
        .clint_base = CLINT_BASE,    .clint_size = CLINT_SIZE,
        .plic_base  = PLIC_BASE,     .plic_size  = PLIC_SIZE,
        .uart_base  = UART_BASE,     .uart_size  = UART_SIZE,
        .uart_irq   = UART_IRQ,      .plic_ndev  = PLIC_NSOURCES - 1,
        .boot_hart  = 0,             .timebase_freq = 10000000,
        .isa        = "rv32ima_zicsr_zifencei",
    };

    uint8_t blob[DTB_MAX_SIZE];
    size_t n = dtb_build(blob, sizeof blob, &cfg);
    if (n == 0 || n + 16 > q->mem.size) return;   /* no room: keep entry defaults */

    uint32_t addr = (q->mem.base + q->mem.size - (uint32_t)n) & ~(uint32_t)7;
    if (addr < q->mem.base + 16) return;          /* paranoia: would underflow RAM */
    if (mem_load(&q->mem, addr, blob, n) != 0) return;
    q->dtb_addr = addr;

    reg_write(&q->cpu, 2, (addr - 16) & ~(uint32_t)0xf); /* sp: 16-aligned, below the DTB */
    reg_write(&q->cpu, 10, cfg.boot_hart);               /* a0 = hartid */
    reg_write(&q->cpu, 11, addr);                        /* a1 = DTB pointer */
}

QuantaStatus quanta_load_elf_ex(Quanta *q, const char *path, uint32_t min_mem) {
    if (!q || !path) return QUANTA_ERR_INVAL;
    uint32_t entry;
    if (elf_load(path, &q->mem, &entry, min_mem) != 0) return QUANTA_ERR_LOAD;
    start_at(q, entry);
    setup_boot(q);   /* hand the guest a device tree per the RISC-V boot contract */
    return QUANTA_OK;
}

QuantaStatus quanta_load_elf(Quanta *q, const char *path) {
    return quanta_load_elf_ex(q, path, 0);
}

QuantaStatus quanta_elf_symbol(const char *path, const char *name,
                               uint32_t *addr) {
    if (!path || !name || !addr) return QUANTA_ERR_INVAL;
    return elf_symbol(path, name, addr) == 0 ? QUANTA_OK : QUANTA_ERR_LOAD;
}

QuantaStatus quanta_load_image(Quanta *q, uint32_t base, uint32_t size,
                               const void *image, size_t len, uint32_t entry) {
    if (!q || (len && !image) || len > size) return QUANTA_ERR_INVAL;
    if (mem_init(&q->mem, base, size) != 0) return QUANTA_ERR_NOMEM;
    if (len && mem_load(&q->mem, base, image, len) != 0) {
        mem_free(&q->mem);
        return QUANTA_ERR_RANGE;
    }
    start_at(q, entry);
    return QUANTA_OK;
}

QuantaStatus quanta_enable_cache(Quanta *q, uint32_t size_bytes,
                                 uint32_t assoc, uint32_t block_size) {
    if (!q || q->cache_on) return QUANTA_ERR_INVAL;
    if (cache_init(&q->cache, size_bytes, assoc, block_size) != 0) {
        return QUANTA_ERR_INVAL;
    }
    q->cache_on = 1;
    if (q->loaded) q->cpu.cache = &q->cache; /* attach if already running */
    return QUANTA_OK;
}

void quanta_cache_report(const Quanta *q, FILE *out) {
    if (q && q->cache_on) cache_report(&q->cache, out);
}

QuantaHalt quanta_step(Quanta *q) {
    if (!q || !q->loaded) return QUANTA_RUN;
    if (!q->cpu.halted) cpu_step(&q->cpu);
    return map_halt(q->cpu.halt_reason);
}

QuantaHalt quanta_run(Quanta *q, uint64_t max_steps, uint64_t *steps_out) {
    uint64_t cap = max_steps ? max_steps : QUANTA_DEFAULT_MAX_STEPS;
    uint64_t steps = 0;
    if (q && q->loaded) {
        while (!q->cpu.halted && steps < cap) {
            cpu_step(&q->cpu);
            steps++;
        }
    }
    if (steps_out) *steps_out = steps;
    if (!q || !q->loaded) return QUANTA_RUN;
    return q->cpu.halted ? map_halt(q->cpu.halt_reason) : QUANTA_HALT_STEP_LIMIT;
}

uint32_t quanta_reg(const Quanta *q, int i) {
    if (!q || i < 0 || i > 31) return 0;
    return reg_read(&q->cpu, (uint32_t)i);
}

void quanta_set_reg(Quanta *q, int i, uint32_t value) {
    if (q && i >= 0 && i <= 31) reg_write(&q->cpu, (uint32_t)i, value);
}

uint32_t quanta_pc(const Quanta *q) { return q ? q->cpu.pc : 0; }
void     quanta_set_pc(Quanta *q, uint32_t pc) { if (q) q->cpu.pc = pc; }

QuantaStatus quanta_mem_read(const Quanta *q, uint32_t addr,
                             void *dst, size_t len) {
    uint32_t off;
    QuantaStatus rc;
    if (!q || (len && !dst)) return QUANTA_ERR_INVAL;
    rc = in_range(&q->mem, addr, len, &off);
    if (rc != QUANTA_OK) return rc;
    memcpy(dst, q->mem.data + off, len);
    return QUANTA_OK;
}

QuantaStatus quanta_mem_write(Quanta *q, uint32_t addr,
                              const void *src, size_t len) {
    uint32_t off;
    QuantaStatus rc;
    if (!q || (len && !src)) return QUANTA_ERR_INVAL;
    rc = in_range(&q->mem, addr, len, &off);
    if (rc != QUANTA_OK) return rc;
    memcpy(q->mem.data + off, src, len);
    return QUANTA_OK;
}

uint32_t quanta_read_u32(const Quanta *q, uint32_t addr, int *ok) {
    uint8_t b[4];
    if (quanta_mem_read(q, addr, b, sizeof b) != QUANTA_OK) {
        if (ok) *ok = 0;
        return 0;
    }
    if (ok) *ok = 1;
    return (uint32_t)b[0]        | (uint32_t)b[1] << 8
         | (uint32_t)b[2] << 16  | (uint32_t)b[3] << 24;
}

QuantaHalt quanta_halt_reason(const Quanta *q) {
    if (!q || !q->cpu.halted) return QUANTA_RUN;
    return map_halt(q->cpu.halt_reason);
}

uint32_t quanta_exit_code(const Quanta *q)  { return q ? q->cpu.exit_code : 0; }
uint32_t quanta_fault_addr(const Quanta *q) { return q ? q->mem.fault_addr : 0; }
uint32_t quanta_mem_base(const Quanta *q)   { return q ? q->mem.base : 0; }
uint32_t quanta_mem_size(const Quanta *q)   { return q ? q->mem.size : 0; }
uint32_t quanta_dtb_addr(const Quanta *q)   { return q ? q->dtb_addr : 0; }

const char *quanta_halt_str(QuantaHalt h) {
    switch (h) {
        case QUANTA_RUN:                  return "running";
        case QUANTA_HALT_EXIT:            return "exit syscall";
        case QUANTA_HALT_EBREAK:          return "ebreak";
        case QUANTA_HALT_ILLEGAL_INSN:    return "illegal instruction";
        case QUANTA_HALT_UNIMP_SYSTEM:    return "unimplemented system instruction";
        case QUANTA_HALT_UNKNOWN_SYSCALL: return "unknown syscall";
        case QUANTA_HALT_MEM_FAULT:       return "memory access out of range";
        case QUANTA_HALT_STEP_LIMIT:      return "instruction-count limit";
    }
    return "unknown";
}

const char *quanta_reg_name(int i) {
    return (i >= 0 && i <= 31) ? reg_abi_name((uint32_t)i) : "?";
}

void quanta_dump_regs(const Quanta *q, FILE *out) {
    if (!q || !out) return;
    fprintf(out, "pc = 0x%08x\n", quanta_pc(q));
    for (int i = 0; i < 32; i++) {
        fprintf(out, "x%-2d %-4s = 0x%08x", i, reg_abi_name((uint32_t)i),
                quanta_reg(q, i));
        fputs((i % 2) ? "\n" : "    ", out);
    }
}
