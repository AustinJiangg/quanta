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
static QuantaStatus in_range(const Memory *mem, uint64_t addr, size_t len,
                            uint64_t *off) {
    if (addr < mem->base || len > mem->size ||
        addr - mem->base > mem->size - (uint64_t)len) {
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
    free(q->plat.disk.data); /* the attached disk image, if any */
    mem_free(&q->mem);
    free(q);
}

/* Shared tail of the loaders: attach the CPU to memory at `entry`, re-attach the
 * cache if one is enabled, and set sp to the top of the region (16-byte aligned
 * per the RISC-V ABI) so the program can call functions and spill locals. */
static void start_at(Quanta *q, uint64_t entry, int xlen) {
    cpu_init(&q->cpu, &q->mem, entry, xlen); /* xlen from the ELF class (raw: 32) */
    if (q->cache_on) q->cpu.cache = &q->cache;
    plat_init(&q->plat);      /* reset the MMIO devices */
    q->mem.plat = &q->plat;   /* attach them so MMIO is dispatched and timers tick */
    plat_attach_ram(&q->plat, q->mem.data, q->mem.base, q->mem.size); /* virtio DMA */
    q->dtb_addr = 0;          /* set only by the boot handoff below (ELF path) */
    reg_write(&q->cpu, 2, (q->mem.base + q->mem.size) & ~(uint64_t)0xf);
    q->loaded = 1;
}

/* Describe this machine for the device-tree builder. Shared by both boot
 * handoffs (Quanta-as-firmware in setup_boot, and an M-mode firmware payload in
 * setup_firmware_boot). `bootargs` is the kernel command line (NULL for none). */
static void dtb_config(const Quanta *q, DtbConfig *cfg, const char *bootargs) {
    int rv64 = (q->cpu.xlen == 64);
    cfg->mem_base   = (uint32_t)q->mem.base; cfg->mem_size = (uint32_t)q->mem.size;
    cfg->test_base  = TEST_BASE;     cfg->test_size  = TEST_SIZE;
    cfg->clint_base = CLINT_BASE;    cfg->clint_size = CLINT_SIZE;
    cfg->plic_base  = PLIC_BASE;     cfg->plic_size  = PLIC_SIZE;
    cfg->uart_base  = UART_BASE;     cfg->uart_size  = UART_SIZE;
    cfg->uart_irq   = UART_IRQ;      cfg->plic_ndev  = PLIC_NSOURCES - 1;
    cfg->boot_hart  = 0;             cfg->timebase_freq = 10000000;
    cfg->bootargs   = bootargs;
    cfg->initrd_start = 0;           cfg->initrd_end = 0; /* set by setup_firmware_boot */
    /* RV64 now walks Sv39 (M18); RV32 uses Sv32. */
    cfg->isa      = rv64 ? "rv64imac_zicsr" : "rv32ima_zicsr_zifencei";
    cfg->mmu_type = rv64 ? "riscv,sv39"     : "riscv,sv32";
}

/* The RISC-V boot handoff (M14): describe this machine in a flattened device
 * tree, drop it at the top of guest memory, and enter the guest the way firmware
 * does — a0 = boot hart id, a1 = the DTB's physical address — with sp moved just
 * below the tree so the stack never grows into it. So a kernel can discover its
 * RAM and devices instead of assuming a fixed layout. The tree is tiny (well
 * under a page) and sits in the loader's stack headroom; if it somehow does not
 * fit, we leave start_at's plain entry state untouched (a0/a1 = 0). */
static void setup_boot(Quanta *q) {
    DtbConfig cfg;
    dtb_config(q, &cfg, NULL);

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

/* The boot handoff when an M-mode firmware (OpenSBI, fw_dynamic) enters the OS
 * instead of Quanta doing it directly. Two differences from setup_boot:
 *
 *  - The DTB is placed LOW, with headroom above it, because OpenSBI expands the
 *    FDT *in place* (fdt_open_into) to add its own nodes (reserved-memory for the
 *    firmware region, PMP, ...). A tree jammed against the top of RAM overflows
 *    the last page — so we park it 2 MiB below the top and leave that slack for
 *    the firmware to grow into.
 *  - We hand the firmware a `fw_dynamic_info` descriptor (magic "OSBI", version 2)
 *    in a2, per OpenSBI's fw_dynamic contract, telling it to enter the payload at
 *    `next_addr` in S-mode. a0 = hartid, a1 = DTB, PC = firmware entry (already
 *    set by start_at). The fields are written explicitly little-endian so the
 *    handoff is host-endianness-independent, like elf.c.
 *
 * Returns 0 on success, -1 if the RAM is too small for the firmware/kernel/DTB
 * layout. */
#define FW_DYNAMIC_MAGIC   0x4942534full  /* "OSBI"                     */
#define FW_DYNAMIC_VERSION 2ull           /* has the boot_hart field    */
#define FW_PRV_S           1ull           /* next_mode = Supervisor     */
#define FW_DTB_HEADROOM    0x200000ull    /* 2 MiB above the DTB to grow */

static int setup_firmware_boot(Quanta *q, uint64_t next_addr, const char *bootargs,
                               const uint8_t *initrd, size_t initrd_size) {
    DtbConfig cfg;
    dtb_config(q, &cfg, bootargs);

    uint64_t top = q->mem.base + q->mem.size;
    uint64_t dtb_addr = (top - FW_DTB_HEADROOM) & ~(uint64_t)7;
    uint64_t info_addr = (dtb_addr - 64) & ~(uint64_t)7; /* just below the DTB */
    if (info_addr <= next_addr) return -1;               /* layout does not fit */

    /* An initramfs, if given, is parked page-aligned just below the fw_dynamic
     * info/DTB — high in RAM, well clear of the kernel image at next_addr, the
     * way qemu places an -initrd. The kernel reserves it from the /chosen bounds
     * we advertise below, so it survives early memory setup. Its physical bounds
     * must be set on cfg before dtb_build so they land in the tree. */
    if (initrd && initrd_size) {
        uint64_t end = info_addr & ~(uint64_t)0xfff;              /* page ceiling */
        uint64_t start = (end - initrd_size) & ~(uint64_t)0xfff;  /* page-aligned base */
        if (start <= next_addr) return -1;                        /* no room */
        if (mem_load(&q->mem, start, initrd, initrd_size) != 0) return -1;
        cfg.initrd_start = (uint32_t)start;
        cfg.initrd_end   = (uint32_t)(start + initrd_size);
    }

    uint8_t blob[DTB_MAX_SIZE];
    size_t n = dtb_build(blob, sizeof blob, &cfg);
    if (n == 0) return -1;
    if (mem_load(&q->mem, dtb_addr, blob, n) != 0) return -1;
    q->dtb_addr = (uint32_t)dtb_addr;

    uint64_t words[6] = { FW_DYNAMIC_MAGIC, FW_DYNAMIC_VERSION,
                          next_addr, FW_PRV_S, 0 /*options*/, 0 /*boot_hart*/ };
    uint8_t info[48];
    for (int i = 0; i < 6; i++)
        for (int b = 0; b < 8; b++)
            info[i * 8 + b] = (uint8_t)(words[i] >> (8 * b));
    if (mem_load(&q->mem, info_addr, info, sizeof info) != 0) return -1;

    reg_write(&q->cpu, 10, cfg.boot_hart); /* a0 = hartid */
    reg_write(&q->cpu, 11, dtb_addr);      /* a1 = DTB */
    reg_write(&q->cpu, 12, info_addr);     /* a2 = &fw_dynamic_info */
    return 0;
}

/* Read a whole file into guest memory at `addr` (a raw image: OpenSBI's S-mode
 * payload or a Linux Image). Mirrors quanta_attach_disk's file handling. */
static QuantaStatus load_raw_file(Quanta *q, const char *path, uint64_t addr) {
    FILE *f = fopen(path, "rb");
    if (!f) return QUANTA_ERR_LOAD;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return QUANTA_ERR_LOAD; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return QUANTA_ERR_LOAD; }

    QuantaStatus st = QUANTA_OK;
    uint8_t *buf = (n > 0) ? malloc((size_t)n) : NULL;
    if (n > 0 && !buf) st = QUANTA_ERR_NOMEM;
    else if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) st = QUANTA_ERR_LOAD;
    else if (mem_load(&q->mem, addr, buf, (size_t)n) != 0) st = QUANTA_ERR_RANGE;
    free(buf);
    fclose(f);
    return st;
}

/* Where an OS payload is loaded above the firmware (the qemu 'virt' convention:
 * 2 MiB above RAM base, so 0x80200000). Linux's Image and OpenSBI's next stage
 * both expect this. */
#define KERNEL_LOAD_OFFSET 0x200000ull

/* Slurp a whole file into a malloc'd buffer (caller frees). Returns QUANTA_OK
 * and sets the out pointer/length, or an error and leaves the pointer NULL. An
 * empty file is an error here — an empty initramfs is not useful. */
static QuantaStatus slurp_file(const char *path, uint8_t **out, size_t *len_out) {
    *out = NULL; *len_out = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return QUANTA_ERR_LOAD;
    QuantaStatus st = QUANTA_OK;
    if (fseek(f, 0, SEEK_END) != 0) st = QUANTA_ERR_LOAD;
    long n = (st == QUANTA_OK) ? ftell(f) : -1;
    if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) st = QUANTA_ERR_LOAD;
    if (st == QUANTA_OK) {
        uint8_t *buf = malloc((size_t)n);
        if (!buf) st = QUANTA_ERR_NOMEM;
        else if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); st = QUANTA_ERR_LOAD; }
        else { *out = buf; *len_out = (size_t)n; }
    }
    fclose(f);
    return st;
}

QuantaStatus quanta_load_firmware(Quanta *q, const char *bios_path,
                                  const char *kernel_path, const char *bootargs,
                                  const char *initrd_path, uint32_t min_mem) {
    if (!q || !bios_path || !kernel_path) return QUANTA_ERR_INVAL;

    uint64_t entry;
    int xlen;
    if (elf_load(bios_path, &q->mem, &entry, &xlen, min_mem) != 0) return QUANTA_ERR_LOAD;
    start_at(q, entry, xlen);

    uint64_t kaddr = q->mem.base + KERNEL_LOAD_OFFSET;
    QuantaStatus st = load_raw_file(q, kernel_path, kaddr);
    if (st != QUANTA_OK) return st;

    uint8_t *initrd = NULL;
    size_t initrd_size = 0;
    if (initrd_path) {
        st = slurp_file(initrd_path, &initrd, &initrd_size);
        if (st != QUANTA_OK) return st;
    }

    int rc = setup_firmware_boot(q, kaddr, bootargs, initrd, initrd_size);
    free(initrd);
    return rc == 0 ? QUANTA_OK : QUANTA_ERR_LOAD;
}

QuantaStatus quanta_load_elf_ex(Quanta *q, const char *path, uint32_t min_mem) {
    if (!q || !path) return QUANTA_ERR_INVAL;
    uint64_t entry;
    int xlen;
    if (elf_load(path, &q->mem, &entry, &xlen, min_mem) != 0) return QUANTA_ERR_LOAD;
    start_at(q, entry, xlen);
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
    start_at(q, entry, 32); /* hand-assembled raw images are RV32 */
    return QUANTA_OK;
}

QuantaStatus quanta_attach_disk(Quanta *q, const char *path) {
    if (!q || !path) return QUANTA_ERR_INVAL;
    FILE *f = fopen(path, "rb");
    if (!f) return QUANTA_ERR_LOAD;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return QUANTA_ERR_LOAD; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return QUANTA_ERR_LOAD; }

    uint8_t *data = NULL;
    if (n > 0) {
        data = malloc((size_t)n);
        if (!data) { fclose(f); return QUANTA_ERR_NOMEM; }
        if (fread(data, 1, (size_t)n, f) != (size_t)n) {
            free(data);
            fclose(f);
            return QUANTA_ERR_LOAD;
        }
    }
    fclose(f);

    free(q->plat.disk.data); /* replace any previously attached disk */
    q->plat.disk.data = data;
    q->plat.disk.size = (uint64_t)n;
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

int quanta_uart_input(Quanta *q, uint8_t byte) {
    if (!q || !q->loaded) return 0;
    return plat_uart_rx(&q->plat, byte);
}

/* The architectural value of an XLEN-wide quantity for API consumers. RV32
 * registers and PC are stored sign-extended into 64 bits internally, so mask to
 * 32 bits to hand back the real 32-bit value (zero-extended); RV64 is unchanged.
 * This also keeps a returned PC a valid address for quanta_read_u32/mem access. */
static uint64_t xlen_val(const Quanta *q, uint64_t v) {
    return q->cpu.xlen == 64 ? v : (v & 0xffffffffu);
}

uint64_t quanta_reg(const Quanta *q, int i) {
    if (!q || i < 0 || i > 31) return 0;
    return xlen_val(q, reg_read(&q->cpu, (uint32_t)i));
}

void quanta_set_reg(Quanta *q, int i, uint64_t value) {
    if (q && i >= 0 && i <= 31) reg_write(&q->cpu, (uint32_t)i, value);
}

uint64_t quanta_pc(const Quanta *q) { return q ? xlen_val(q, q->cpu.pc) : 0; }
void     quanta_set_pc(Quanta *q, uint64_t pc) { if (q) q->cpu.pc = pc; }
int      quanta_xlen(const Quanta *q) { return (q && q->loaded) ? q->cpu.xlen : 0; }

QuantaStatus quanta_mem_read(const Quanta *q, uint64_t addr,
                             void *dst, size_t len) {
    uint64_t off;
    QuantaStatus rc;
    if (!q || (len && !dst)) return QUANTA_ERR_INVAL;
    rc = in_range(&q->mem, addr, len, &off);
    if (rc != QUANTA_OK) return rc;
    memcpy(dst, q->mem.data + off, len);
    return QUANTA_OK;
}

QuantaStatus quanta_mem_write(Quanta *q, uint64_t addr,
                              const void *src, size_t len) {
    uint64_t off;
    QuantaStatus rc;
    if (!q || (len && !src)) return QUANTA_ERR_INVAL;
    rc = in_range(&q->mem, addr, len, &off);
    if (rc != QUANTA_OK) return rc;
    memcpy(q->mem.data + off, src, len);
    return QUANTA_OK;
}

uint32_t quanta_read_u32(const Quanta *q, uint64_t addr, int *ok) {
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
uint64_t quanta_fault_addr(const Quanta *q) { return q ? xlen_val(q, q->mem.fault_addr) : 0; }
uint64_t quanta_mem_base(const Quanta *q)   { return q ? q->mem.base : 0; }
uint64_t quanta_mem_size(const Quanta *q)   { return q ? q->mem.size : 0; }
uint64_t quanta_dtb_addr(const Quanta *q)   { return q ? q->dtb_addr : 0; }

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
    int w = (quanta_xlen(q) == 64) ? 16 : 8; /* hex digits per XLEN-wide value */
    fprintf(out, "pc = 0x%0*llx\n", w, (unsigned long long)quanta_pc(q));
    for (int i = 0; i < 32; i++) {
        fprintf(out, "x%-2d %-4s = 0x%0*llx", i, reg_abi_name((uint32_t)i),
                w, (unsigned long long)quanta_reg(q, i));
        fputs((i % 2) ? "\n" : "    ", out);
    }
}
