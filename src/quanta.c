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
 * The handle owns every piece of the machine. Each CPU (hart) holds a borrowed
 * pointer to the shared Memory (and, when enabled, the Cache); with SMP (M19)
 * several harts share that one memory and the one Platform, and a round-robin
 * scheduler interleaves them one instruction at a time on the host thread — a
 * deterministic model of concurrency. harts[0] is the boot hart; nharts == 1 is
 * the ordinary uniprocessor.
 */
struct Quanta {
    CPU      harts[QUANTA_MAX_HARTS];
    int      nharts;    /* number of active harts (1..QUANTA_MAX_HARTS) */
    int      sched;     /* round-robin cursor for the incremental quanta_step */
    int      m_halted;      /* the whole machine has stopped */
    HaltReason m_reason;    /* why (meaningful once m_halted) */
    uint32_t m_exit;        /* machine exit status */
    Memory   mem;
    Cache    cache;
    Platform plat;    /* MMIO devices: CLINT timer/IPI, PLIC, 16550 UART (M13) */
    uint32_t dtb_addr;  /* where the boot device tree was placed (0 if none, M14) */
    int      cache_on;  /* a cache has been attached */
    int      loaded;    /* memory is initialised and PC/sp are set */
    int      netdev_advertised; /* emit the virtio-net node in the boot DTB (M23) */
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
    Quanta *q = calloc(1, sizeof(Quanta)); /* NULL on OOM */
    if (q) q->nharts = 1;                  /* a uniprocessor unless --harts asks for more */
    return q;
}

QuantaStatus quanta_set_harts(Quanta *q, int nharts) {
    if (!q || q->loaded) return QUANTA_ERR_INVAL;      /* set before loading */
    if (nharts < 1 || nharts > (int)QUANTA_MAX_HARTS) return QUANTA_ERR_INVAL;
    q->nharts = nharts;
    return QUANTA_OK;
}

/* Update the machine-level halt state, polled after every hart step. The whole
 * machine stops on a global power-off (the SiFive test device / SBI SRST routed
 * through the platform) or once every hart has individually halted. On all-halted
 * an abnormal hart reason (a fault) is surfaced ahead of a clean exit, so a
 * secondary hart's crash is not masked by the boot hart's clean stop. */
static void machine_poll_halt(Quanta *q) {
    uint32_t code;
    if (plat_poweroff_requested(&q->plat, &code)) {
        q->m_halted = 1; q->m_reason = HALT_EXIT; q->m_exit = code;
        return;
    }
    int abnormal = -1;
    for (int i = 0; i < q->nharts; i++) {
        if (!q->harts[i].halted) return;               /* someone is still running */
        if (q->harts[i].halt_reason != HALT_EXIT && abnormal < 0) abnormal = i;
    }
    int who = (abnormal >= 0) ? abnormal : 0;
    q->m_halted = 1;
    q->m_reason = q->harts[who].halt_reason;
    q->m_exit   = q->harts[who].exit_code;
}

void quanta_destroy(Quanta *q) {
    if (!q) return;
    if (q->cache_on) cache_free(&q->cache);
    if (q->plat.disk.file) {           /* flush write-through and release the handle */
        fflush(q->plat.disk.file);
        fclose(q->plat.disk.file);
    }
    free(q->plat.disk.data); /* the attached disk image, if any */
    mem_free(&q->mem);
    free(q);
}

/* Shared tail of the loaders: reset the platform, then bring up every hart at
 * `entry` sharing the one memory. Each hart gets its id in mhartid (and a0 later,
 * at the boot handoff), the cache if one is enabled, and sp at the top of the
 * region (16-byte aligned per the RISC-V ABI) — an SMP guest repartitions its own
 * per-hart stacks early using mhartid. All harts start running; the firmware boot
 * path parks the secondaries (see setup_firmware_boot). */
static void start_at(Quanta *q, uint64_t entry, int xlen) {
    if (q->nharts < 1) q->nharts = 1;
    plat_init(&q->plat);      /* reset the MMIO devices */
    q->mem.plat = &q->plat;   /* attach them so MMIO is dispatched and timers tick */
    plat_attach_ram(&q->plat, q->mem.data, q->mem.base, q->mem.size); /* virtio DMA */
    plat_set_harts(&q->plat, q->harts, q->nharts); /* CLINT/atomics reach every hart */

    uint64_t sp = (q->mem.base + q->mem.size) & ~(uint64_t)0xf;
    for (int i = 0; i < q->nharts; i++) {
        cpu_init(&q->harts[i], &q->mem, entry, xlen); /* xlen from the ELF class (raw: 32) */
        q->harts[i].hartid = (uint32_t)i;
        q->harts[i].csr[CSR_MHARTID] = (uint64_t)i;
        if (q->cache_on) q->harts[i].cache = &q->cache;
        reg_write(&q->harts[i], 2, sp);
    }
    q->dtb_addr = 0;          /* set only by the boot handoff below (ELF path) */
    q->sched = 0;
    q->m_halted = 0; q->m_reason = HALT_NONE; q->m_exit = 0;
    q->loaded = 1;
}

/* Describe this machine for the device-tree builder. Shared by both boot
 * handoffs (Quanta-as-firmware in setup_boot, and an M-mode firmware payload in
 * setup_firmware_boot). `bootargs` is the kernel command line (NULL for none). */
static void dtb_config(const Quanta *q, DtbConfig *cfg, const char *bootargs) {
    int rv64 = (q->harts[0].xlen == 64);
    cfg->mem_base   = (uint32_t)q->mem.base; cfg->mem_size = (uint32_t)q->mem.size;
    cfg->test_base  = TEST_BASE;     cfg->test_size  = TEST_SIZE;
    cfg->clint_base = CLINT_BASE;    cfg->clint_size = CLINT_SIZE;
    cfg->plic_base  = PLIC_BASE;     cfg->plic_size  = PLIC_SIZE;
    cfg->uart_base  = UART_BASE;     cfg->uart_size  = UART_SIZE;
    cfg->uart_irq   = UART_IRQ;      cfg->plic_ndev  = PLIC_NSOURCES - 1;
    cfg->boot_hart  = 0;             cfg->timebase_freq = 10000000;
    cfg->nharts     = (uint32_t)q->nharts;
    cfg->bootargs   = bootargs;
    cfg->initrd_start = 0;           cfg->initrd_end = 0; /* set by setup_firmware_boot */
    /* Advertise the virtio-net device only when --netdev attached a backend (M23),
     * so a guest OS discovers it exactly when it is usable. */
    if (q->netdev_advertised) {
        cfg->virtio_net_base = VIRTIO_NET_BASE; cfg->virtio_net_size = VIRTIO_NET_SIZE;
        cfg->virtio_net_irq  = VIRTIO_NET_IRQ;
    } else {
        cfg->virtio_net_base = 0; cfg->virtio_net_size = 0; cfg->virtio_net_irq = 0;
    }
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

    /* Enter every hart the way qemu's `-bios none` does: PC at the entry (set by
     * start_at), a0 = this hart's id, a1 = the DTB, sp below the tree. An SMP OS
     * (xv6 CPUS>1) dispatches on a0/mhartid from here. */
    uint64_t new_sp = (addr - 16) & ~(uint64_t)0xf;
    for (int i = 0; i < q->nharts; i++) {
        reg_write(&q->harts[i], 2, new_sp);          /* sp: 16-aligned, below the DTB */
        reg_write(&q->harts[i], 10, (uint64_t)i);    /* a0 = this hart's id */
        reg_write(&q->harts[i], 11, addr);           /* a1 = DTB pointer */
    }
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

    /* SMP under an M-mode firmware (OpenSBI, then Linux SMP): every hart enters
     * the firmware at reset with the boot registers set (a0 = its own hartid, a1 =
     * DTB, a2 = &fw_dynamic_info). OpenSBI's boot-hart lottery lets the designated
     * boot hart (fw_dynamic boot_hart, here 0) cold-boot and jump to the OS, while
     * the others fall into OpenSBI's warm-boot / HSM wait loop until Linux releases
     * each via SBI hart_start (a CLINT IPI). So the secondaries run from reset
     * rather than being parked. */
    for (int i = 0; i < q->nharts; i++) {
        reg_write(&q->harts[i], 10, (uint64_t)i);  /* a0 = this hart's id */
        reg_write(&q->harts[i], 11, dtb_addr);     /* a1 = DTB */
        reg_write(&q->harts[i], 12, info_addr);    /* a2 = &fw_dynamic_info */
    }
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

QuantaStatus quanta_attach_disk_ex(Quanta *q, const char *path, int writable) {
    if (!q || !path) return QUANTA_ERR_INVAL;
    FILE *f = fopen(path, writable ? "r+b" : "rb");
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
    /* Read-only: the image is now cached in RAM, so drop the handle — writes will
     * hit the RAM copy and be discarded. Writable: keep it open "r+b" for the block
     * device's write-through (the initial read above leaves the position at EOF; the
     * device fseeks before each write, so the read->write switch stays conforming). */
    if (!writable) { fclose(f); f = NULL; }

    if (q->plat.disk.file) fclose(q->plat.disk.file); /* release a previous handle */
    free(q->plat.disk.data);                          /* replace any previous image  */
    q->plat.disk.data     = data;
    q->plat.disk.size     = (uint64_t)n;
    q->plat.disk.file     = f;
    q->plat.disk.writable = (f != NULL);
    return QUANTA_OK;
}

QuantaStatus quanta_attach_disk(Quanta *q, const char *path) {
    return quanta_attach_disk_ex(q, path, 0);
}

QuantaStatus quanta_enable_cache(Quanta *q, uint32_t size_bytes,
                                 uint32_t assoc, uint32_t block_size) {
    if (!q || q->cache_on) return QUANTA_ERR_INVAL;
    if (cache_init(&q->cache, size_bytes, assoc, block_size) != 0) {
        return QUANTA_ERR_INVAL;
    }
    q->cache_on = 1;
    if (q->loaded)                                /* attach to every hart if running */
        for (int i = 0; i < q->nharts; i++) q->harts[i].cache = &q->cache;
    return QUANTA_OK;
}

void quanta_cache_report(const Quanta *q, FILE *out) {
    if (q && q->cache_on) cache_report(&q->cache, out);
}

/* Advance the machine by one hart-instruction, round-robin across the harts —
 * the incremental stepper the CLI's trace loop and the GDB stub drive. The shared
 * platform timer ticks once at the start of each round (cursor back at hart 0), so
 * mtime advances at one rate whatever the hart count; on a uniprocessor this is
 * exactly "tick, then step hart 0" as before. A halted hart's slot is a no-op. */
QuantaHalt quanta_step(Quanta *q) {
    if (!q || !q->loaded) return QUANTA_RUN;
    if (q->m_halted) return map_halt(q->m_reason);
    if (q->sched == 0) plat_tick(&q->plat);
    CPU *h = &q->harts[q->sched];
    if (!h->halted) cpu_step(h);
    q->sched = (q->sched + 1) % q->nharts;
    machine_poll_halt(q);
    return q->m_halted ? map_halt(q->m_reason) : QUANTA_RUN;
}

QuantaHalt quanta_run(Quanta *q, uint64_t max_steps, uint64_t *steps_out) {
    uint64_t cap = max_steps ? max_steps : QUANTA_DEFAULT_MAX_STEPS;
    uint64_t steps = 0;
    if (q && q->loaded) {
        while (!q->m_halted && steps < cap) {
            if (q->sched == 0) plat_tick(&q->plat);   /* one tick per scheduler round */
            CPU *h = &q->harts[q->sched];
            if (!h->halted) { cpu_step(h); steps++; } /* count only real instructions */
            q->sched = (q->sched + 1) % q->nharts;
            machine_poll_halt(q);
        }
    }
    if (steps_out) *steps_out = steps;
    if (!q || !q->loaded) return QUANTA_RUN;
    return q->m_halted ? map_halt(q->m_reason) : QUANTA_HALT_STEP_LIMIT;
}

int quanta_uart_input(Quanta *q, uint8_t byte) {
    if (!q || !q->loaded) return 0;
    return plat_uart_rx(&q->plat, byte);
}

int quanta_net_rx(Quanta *q, const uint8_t *frame, uint32_t len) {
    if (!q || !q->loaded || !frame) return 0;
    return plat_net_rx(&q->plat, frame, len);
}

void quanta_net_set_backend(Quanta *q,
                            void (*tx)(void *ctx, const uint8_t *frame, uint32_t len),
                            void *ctx) {
    if (!q) return;
    plat_net_set_backend(&q->plat, tx, ctx);
}

void quanta_set_netdev_advertised(Quanta *q, int on) {
    if (q) q->netdev_advertised = on ? 1 : 0;
}

/*
 * Snapshot / restore (E10). The whole mutable machine is a fixed-size struct plus
 * two heap buffers (guest RAM and the optional in-memory disk); everything else —
 * the harts (registers/CSRs/TLB), the device register files, and the scheduler
 * bookkeeping — lives inline in the handle. So a snapshot is a value copy of the
 * inline state plus a deep copy of the two buffers, and a restore reverses it and
 * re-points the borrowed pointers (each hart's memory/cache, the memory's platform
 * back-pointer, and the platform's RAM/hart/disk pointers) at the live objects.
 *
 * The cache is left out on purpose: it is a pure observability layer whose state
 * never changes results, so a restored run stays bit-identical without it, and
 * copying its internal LRU arrays would only add cost. The determinism of the
 * round-robin scheduler is what makes this exact — a restored machine re-executes
 * the same instruction stream given the same subsequent console input.
 */
struct QuantaSnapshot {
    CPU        harts[QUANTA_MAX_HARTS]; /* value copy; borrowed pointers rewired on restore */
    int        nharts;
    int        sched;
    int        m_halted;
    HaltReason m_reason;
    uint32_t   m_exit;
    uint32_t   dtb_addr;
    int        cache_on;
    int        loaded;

    uint64_t   mem_base;
    uint64_t   mem_size;
    int        mem_fault;
    uint64_t   mem_fault_addr;
    uint8_t   *mem_data;   /* deep copy of mem_size bytes */

    Platform   plat;       /* value copy of the device register files; pointers rewired */
    uint8_t   *disk_data;  /* deep copy of disk_size bytes, or NULL when no disk */
    uint64_t   disk_size;
};

QuantaSnapshot *quanta_snapshot(const Quanta *q) {
    if (!q || !q->loaded) return NULL;

    QuantaSnapshot *s = calloc(1, sizeof *s);
    if (!s) return NULL;

    memcpy(s->harts, q->harts, sizeof s->harts);
    s->nharts   = q->nharts;   s->sched    = q->sched;
    s->m_halted = q->m_halted; s->m_reason = q->m_reason; s->m_exit = q->m_exit;
    s->dtb_addr = q->dtb_addr; s->cache_on = q->cache_on; s->loaded = q->loaded;

    s->mem_base  = q->mem.base;  s->mem_size = q->mem.size;
    s->mem_fault = q->mem.fault; s->mem_fault_addr = q->mem.fault_addr;
    s->mem_data  = malloc(q->mem.size ? q->mem.size : 1);
    if (!s->mem_data) { free(s); return NULL; }
    memcpy(s->mem_data, q->mem.data, q->mem.size);

    s->plat      = q->plat;   /* register files + pointer *values* (rewired on restore) */
    s->disk_size = q->plat.disk.size;
    if (q->plat.disk.data && q->plat.disk.size) {
        s->disk_data = malloc(q->plat.disk.size);
        if (!s->disk_data) { free(s->mem_data); free(s); return NULL; }
        memcpy(s->disk_data, q->plat.disk.data, q->plat.disk.size);
    }
    return s;
}

QuantaStatus quanta_restore(Quanta *q, const QuantaSnapshot *s) {
    if (!q || !s || !q->loaded || !s->loaded) return QUANTA_ERR_INVAL;
    /* Same-machine only: the live heap buffers must match the snapshot's sizes,
     * since we restore into them in place rather than reallocating. */
    if (s->mem_size != q->mem.size || s->disk_size != q->plat.disk.size)
        return QUANTA_ERR_INVAL;

    uint8_t *live_ram  = q->mem.data;        /* engine-owned buffers to keep */
    uint8_t *live_disk = q->plat.disk.data;
    FILE    *live_file = q->plat.disk.file;  /* the --disk backing handle, not in the snapshot */
    int      live_wr   = q->plat.disk.writable;

    /* Harts: value copy, then re-point each hart's borrowed pointers at the live
     * memory and (if enabled) cache. */
    memcpy(q->harts, s->harts, sizeof q->harts);
    for (unsigned i = 0; i < QUANTA_MAX_HARTS; i++) {
        q->harts[i].mem   = &q->mem;
        q->harts[i].cache = s->cache_on ? &q->cache : NULL;
    }

    q->nharts   = s->nharts;   q->sched    = s->sched;
    q->m_halted = s->m_halted; q->m_reason = s->m_reason; q->m_exit = s->m_exit;
    q->dtb_addr = s->dtb_addr; q->cache_on = s->cache_on; q->loaded = s->loaded;

    /* Memory: restore contents into the live buffer, keep the live pointer, and
     * re-attach the platform back-pointer. */
    memcpy(live_ram, s->mem_data, s->mem_size);
    q->mem.base = s->mem_base; q->mem.size = s->mem_size;
    q->mem.fault = s->mem_fault; q->mem.fault_addr = s->mem_fault_addr;
    q->mem.data = live_ram;
    q->mem.plat = &q->plat;

    /* Platform: value copy of the device register files, then re-wire every
     * pointer to a live object and restore the disk contents in place. */
    q->plat = s->plat;
    q->plat.harts    = q->harts;
    q->plat.nharts   = s->nharts;
    q->plat.ram      = live_ram;
    q->plat.ram_base = q->mem.base;
    q->plat.ram_size = q->mem.size;
    q->plat.disk.data = live_disk;
    q->plat.disk.size = s->disk_size;
    q->plat.disk.file = live_file;     /* the snapshot never captured the host handle */
    q->plat.disk.writable = live_wr;
    if (live_disk && s->disk_data) memcpy(live_disk, s->disk_data, s->disk_size);

    return QUANTA_OK;
}

void quanta_snapshot_free(QuantaSnapshot *s) {
    if (!s) return;
    free(s->mem_data);
    free(s->disk_data);
    free(s);
}

/*
 * Snapshot file serialisation (E10, --snapshot / --restore). The format is a
 * fixed 72-byte little-endian header, then the raw bytes of the fixed-size inline
 * state (the harts array and the Platform), then guest RAM and the disk image.
 *
 * The inline state is written as raw structs rather than field-by-field: the CSR
 * file alone is 4096 entries per hart, so an explicit serialiser would be enormous
 * and no more correct on the same build. The trade-off is that a snapshot file is
 * host-ABI specific — the header therefore records sizeof(CPU), sizeof(Platform),
 * and QUANTA_MAX_HARTS, and a load rejects any file whose signature does not match
 * the running binary, so a mismatched build fails cleanly instead of mis-reading.
 * The pointer members inside those structs are meaningless on disk; quanta_restore
 * re-wires them to the live objects, exactly as for an in-memory snapshot.
 */
static const uint8_t SNAP_MAGIC[8] = { 'Q','U','A','N','T','A','S','N' };
#define SNAP_FORMAT_VERSION 1u
#define SNAP_HDR_SIZE       72u   /* 8 magic + 10*u32 + 3*u64 */

static size_t put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
    return 4;
}
static size_t put_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
    return 8;
}
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static QuantaStatus snapshot_write(const QuantaSnapshot *s, FILE *f) {
    uint8_t hdr[SNAP_HDR_SIZE];
    size_t o = 0;
    memcpy(hdr + o, SNAP_MAGIC, 8);                          o += 8;
    o += put_u32(hdr + o, SNAP_FORMAT_VERSION);
    o += put_u32(hdr + o, (uint32_t)sizeof(CPU));
    o += put_u32(hdr + o, (uint32_t)sizeof(Platform));
    o += put_u32(hdr + o, (uint32_t)QUANTA_MAX_HARTS);
    o += put_u32(hdr + o, (uint32_t)s->nharts);
    o += put_u32(hdr + o, (uint32_t)s->sched);
    o += put_u32(hdr + o, (uint32_t)s->m_halted);
    o += put_u32(hdr + o, (uint32_t)s->m_reason);
    o += put_u32(hdr + o, s->m_exit);
    o += put_u32(hdr + o, s->dtb_addr);
    o += put_u64(hdr + o, s->mem_base);
    o += put_u64(hdr + o, s->mem_size);
    o += put_u64(hdr + o, s->disk_size);

    if (fwrite(hdr, 1, o, f) != o) return QUANTA_ERR_LOAD;
    if (fwrite(s->harts, sizeof s->harts, 1, f) != 1) return QUANTA_ERR_LOAD;
    if (fwrite(&s->plat, sizeof s->plat, 1, f) != 1) return QUANTA_ERR_LOAD;
    if (s->mem_size &&
        fwrite(s->mem_data, 1, s->mem_size, f) != s->mem_size) return QUANTA_ERR_LOAD;
    if (s->disk_size &&
        fwrite(s->disk_data, 1, s->disk_size, f) != s->disk_size) return QUANTA_ERR_LOAD;
    return QUANTA_OK;
}

/* Read a snapshot file into a fresh QuantaSnapshot (RAM/disk buffers allocated and
 * filled). Returns NULL on any I/O error, a bad magic/version, or a layout
 * signature that does not match this binary. */
static QuantaSnapshot *snapshot_read(FILE *f) {
    uint8_t hdr[SNAP_HDR_SIZE];
    if (fread(hdr, 1, SNAP_HDR_SIZE, f) != SNAP_HDR_SIZE) return NULL;
    if (memcmp(hdr, SNAP_MAGIC, 8) != 0) return NULL;
    size_t o = 8;
    uint32_t ver   = get_u32(hdr + o); o += 4;
    uint32_t szcpu = get_u32(hdr + o); o += 4;
    uint32_t szpl  = get_u32(hdr + o); o += 4;
    uint32_t maxh  = get_u32(hdr + o); o += 4;
    if (ver != SNAP_FORMAT_VERSION || szcpu != sizeof(CPU) ||
        szpl != sizeof(Platform) || maxh != QUANTA_MAX_HARTS)
        return NULL;   /* written by an incompatible build */

    QuantaSnapshot *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->nharts   = (int)get_u32(hdr + o); o += 4;
    s->sched    = (int)get_u32(hdr + o); o += 4;
    s->m_halted = (int)get_u32(hdr + o); o += 4;
    s->m_reason = (HaltReason)get_u32(hdr + o); o += 4;
    s->m_exit   = get_u32(hdr + o); o += 4;
    s->dtb_addr = get_u32(hdr + o); o += 4;
    s->mem_base = get_u64(hdr + o); o += 8;
    s->mem_size = get_u64(hdr + o); o += 8;
    s->disk_size = get_u64(hdr + o);
    s->cache_on = 0;     /* the cache is observability-only; never persisted */
    s->loaded   = 1;

    if (s->nharts < 1 || s->nharts > (int)QUANTA_MAX_HARTS) { free(s); return NULL; }
    if (fread(s->harts, sizeof s->harts, 1, f) != 1) { free(s); return NULL; }
    if (fread(&s->plat, sizeof s->plat, 1, f) != 1) { free(s); return NULL; }

    s->mem_data = malloc(s->mem_size ? s->mem_size : 1);
    if (!s->mem_data) { free(s); return NULL; }
    if (s->mem_size && fread(s->mem_data, 1, s->mem_size, f) != s->mem_size) {
        free(s->mem_data); free(s); return NULL;
    }
    if (s->disk_size) {
        s->disk_data = malloc(s->disk_size);
        if (!s->disk_data) { free(s->mem_data); free(s); return NULL; }
        if (fread(s->disk_data, 1, s->disk_size, f) != s->disk_size) {
            free(s->disk_data); free(s->mem_data); free(s); return NULL;
        }
    }
    return s;
}

QuantaStatus quanta_save_snapshot(const Quanta *q, const char *path) {
    if (!q || !path) return QUANTA_ERR_INVAL;
    if (!q->loaded) return QUANTA_ERR_INVAL;

    QuantaSnapshot *s = quanta_snapshot(q);
    if (!s) return QUANTA_ERR_NOMEM;

    FILE *f = fopen(path, "wb");
    if (!f) { quanta_snapshot_free(s); return QUANTA_ERR_LOAD; }
    QuantaStatus st = snapshot_write(s, f);
    if (fclose(f) != 0 && st == QUANTA_OK) st = QUANTA_ERR_LOAD; /* catch flush errors */
    quanta_snapshot_free(s);
    return st;
}

QuantaStatus quanta_load_snapshot(Quanta *q, const char *path) {
    if (!q || !path) return QUANTA_ERR_INVAL;
    if (q->loaded) return QUANTA_ERR_INVAL;   /* load into a fresh handle */

    FILE *f = fopen(path, "rb");
    if (!f) return QUANTA_ERR_LOAD;
    QuantaSnapshot *s = snapshot_read(f);
    fclose(f);
    if (!s) return QUANTA_ERR_LOAD;

    /* Build a live machine matching the snapshot's sizes, then restore into it —
     * reusing quanta_restore's pointer re-wiring. mem_init/the disk alloc give the
     * live buffers quanta_restore fills in place. */
    if (mem_init(&q->mem, s->mem_base, s->mem_size) != 0) {
        quanta_snapshot_free(s);
        return QUANTA_ERR_NOMEM;
    }
    if (s->disk_size) {
        q->plat.disk.data = malloc(s->disk_size);
        if (!q->plat.disk.data) {
            mem_free(&q->mem);
            quanta_snapshot_free(s);
            return QUANTA_ERR_NOMEM;
        }
        q->plat.disk.size = s->disk_size;
    }
    /* A restored checkpoint has no --disk backing file: writes hit the in-RAM image
     * and are discarded. quanta_restore preserves these live-engine fields. */
    q->plat.disk.file     = NULL;
    q->plat.disk.writable = 0;
    q->nharts = s->nharts;
    q->loaded = 1;   /* so quanta_restore's guard passes */

    QuantaStatus st = quanta_restore(q, s);
    quanta_snapshot_free(s);
    if (st != QUANTA_OK) {   /* shouldn't happen — sizes match by construction */
        mem_free(&q->mem);
        free(q->plat.disk.data);
        q->plat.disk.data = NULL;
        q->plat.disk.size = 0;
        q->loaded = 0;
    }
    return st;
}

/* The architectural value of an XLEN-wide quantity for API consumers. RV32
 * registers and PC are stored sign-extended into 64 bits internally, so mask to
 * 32 bits to hand back the real 32-bit value (zero-extended); RV64 is unchanged.
 * This also keeps a returned PC a valid address for quanta_read_u32/mem access. */
static uint64_t xlen_val(const Quanta *q, uint64_t v) {
    return q->harts[0].xlen == 64 ? v : (v & 0xffffffffu);
}

uint64_t quanta_reg(const Quanta *q, int i) {
    if (!q || i < 0 || i > 31) return 0;
    return xlen_val(q, reg_read(&q->harts[0], (uint32_t)i));
}

void quanta_set_reg(Quanta *q, int i, uint64_t value) {
    if (q && i >= 0 && i <= 31) reg_write(&q->harts[0], (uint32_t)i, value);
}

uint64_t quanta_pc(const Quanta *q) { return q ? xlen_val(q, q->harts[0].pc) : 0; }
void     quanta_set_pc(Quanta *q, uint64_t pc) { if (q) q->harts[0].pc = pc; }
int      quanta_xlen(const Quanta *q) { return (q && q->loaded) ? q->harts[0].xlen : 0; }

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
    if (!q || !q->m_halted) return QUANTA_RUN;
    return map_halt(q->m_reason);
}

uint32_t quanta_exit_code(const Quanta *q)  { return q ? q->m_exit : 0; }
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
