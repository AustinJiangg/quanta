#include "dtb.h"

#include <stdio.h>
#include <string.h>

/*
 * Flattened device tree serialiser — see dtb.h for the why.
 *
 * The on-disk FDT (the binary a `dtc` compiler emits) is four regions laid out
 * back to back, all multi-byte fields big-endian:
 *
 *   [ header (40 bytes) ][ memory reservation block ][ struct block ][ strings ]
 *
 * The struct block is a stream of 32-bit tokens: BEGIN_NODE (then a NUL-padded
 * name), PROP (then len, a name offset into the strings block, and the padded
 * value), END_NODE, and a final END. Property names are deduplicated into the
 * strings block and referenced by offset. We build the struct block straight
 * into the output buffer and accumulate the strings block separately, then
 * append it and back-fill the header once the sizes are known.
 *
 * The format is specified by the Devicetree Specification; the constants below
 * are its magic numbers.
 */

#define FDT_MAGIC       0xd00dfeedu
#define FDT_VERSION     17u  /* the version we emit */
#define FDT_LAST_COMP   16u  /* oldest version a reader needs to understand it */

#define FDT_BEGIN_NODE  0x1u
#define FDT_END_NODE    0x2u
#define FDT_PROP        0x3u
#define FDT_NOP         0x4u  /* unused here, listed for completeness */
#define FDT_END         0x9u

#define HEADER_SIZE     40u
#define RSVMAP_SIZE     16u                       /* one terminating (0,0) entry */
#define STRUCT_OFF      (HEADER_SIZE + RSVMAP_SIZE) /* 56: 8-aligned, 4-aligned  */

/* phandle ids referenced by the interrupt wiring (any nonzero unique values).
 * Each hart's local interrupt controller gets its own phandle, from a base high
 * enough not to collide with the PLIC's (SMP, M19). */
#define PHANDLE_PLIC     2u          /* the platform-level interrupt controller */
#define PHANDLE_CPU_INTC(h) (16u + (h))  /* hart h's local interrupt controller */

/* Builder state. The struct block is written into out[STRUCT_OFF..]; strings
 * accumulate in a side buffer until finalisation. `ok` latches to 0 on the
 * first write that would exceed a buffer, so a single check at the end suffices. */
typedef struct {
    uint8_t *out;
    size_t   cap;
    size_t   spos;          /* next write offset into out (the struct cursor) */
    char     strings[512];
    size_t   slen;
    int      ok;
} Fdt;

/* Store a big-endian 32-bit word at p. */
static void wr_be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Append a big-endian word to the struct block. */
static void put_u32(Fdt *f, uint32_t v) {
    if (f->spos + 4 > f->cap) { f->ok = 0; return; }
    wr_be(f->out + f->spos, v);
    f->spos += 4;
}

/* Append raw bytes to the struct block. */
static void put_bytes(Fdt *f, const void *p, size_t n) {
    if (n == 0) return;                       /* nothing to copy (avoid a NULL memcpy) */
    if (f->spos + n > f->cap) { f->ok = 0; return; }
    memcpy(f->out + f->spos, p, n);
    f->spos += n;
}

/* Zero-pad the struct cursor up to the next 4-byte boundary. */
static void pad_struct(Fdt *f) {
    while (f->spos & 3u) {
        if (f->spos + 1 > f->cap) { f->ok = 0; return; }
        f->out[f->spos++] = 0;
    }
}

/* Intern a property name into the strings block, returning its byte offset.
 * Names recur across nodes (reg, compatible, ...), so dedup keeps the blob small
 * and matches what dtc produces. */
static uint32_t intern(Fdt *f, const char *name) {
    for (size_t i = 0; i < f->slen; i += strlen(f->strings + i) + 1) {
        if (strcmp(f->strings + i, name) == 0) return (uint32_t)i;
    }
    size_t n = strlen(name) + 1;
    if (f->slen + n > sizeof f->strings) { f->ok = 0; return 0; }
    memcpy(f->strings + f->slen, name, n);
    uint32_t off = (uint32_t)f->slen;
    f->slen += n;
    return off;
}

static void begin_node(Fdt *f, const char *name) {
    put_u32(f, FDT_BEGIN_NODE);
    put_bytes(f, name, strlen(name) + 1);
    pad_struct(f);
}

static void end_node(Fdt *f) {
    put_u32(f, FDT_END_NODE);
}

/* A property with an arbitrary byte value. */
static void prop(Fdt *f, const char *name, const void *val, uint32_t len) {
    put_u32(f, FDT_PROP);
    put_u32(f, len);
    put_u32(f, intern(f, name));
    put_bytes(f, val, len);
    pad_struct(f);
}

/* A valueless (boolean) property, e.g. `ranges` or `interrupt-controller`. */
static void prop_empty(Fdt *f, const char *name) {
    prop(f, name, NULL, 0);
}

/* A string property (value includes the terminating NUL, as the format wants). */
static void prop_str(Fdt *f, const char *name, const char *s) {
    prop(f, name, s, (uint32_t)(strlen(s) + 1));
}

/* A property holding `n` 32-bit cells, each serialised big-endian. */
static void prop_cells(Fdt *f, const char *name, const uint32_t *cells, uint32_t n) {
    put_u32(f, FDT_PROP);
    put_u32(f, n * 4u);
    put_u32(f, intern(f, name));
    for (uint32_t i = 0; i < n; i++) put_u32(f, cells[i]);
    pad_struct(f);
}

/* A single-cell property (the common case: #address-cells, reg id, phandle). */
static void prop_u32(Fdt *f, const char *name, uint32_t v) {
    prop_cells(f, name, &v, 1);
}

/* reg = <0 base 0 size> under the root's #address-cells=2 / #size-cells=2: a
 * 64-bit address and size carried as cell pairs, the high cell always 0 on our
 * 32-bit machine. */
static void prop_reg(Fdt *f, uint32_t base, uint32_t size) {
    uint32_t cells[4] = { 0, base, 0, size };
    prop_cells(f, "reg", cells, 4);
}

size_t dtb_build(uint8_t *buf, size_t cap, const DtbConfig *cfg) {
    if (!buf || !cfg || cap < STRUCT_OFF) return 0;

    /* Zero the whole buffer up front: this lays down the header placeholder and,
     * importantly, the memory-reservation block — a single terminating entry of
     * two 64-bit zeros at offset HEADER_SIZE, meaning "no regions reserved". */
    memset(buf, 0, cap);

    Fdt f = { buf, cap, STRUCT_OFF, { 0 }, 0, 1 };

    char unit[32];  /* node names with a unit address: "memory@80000000", ... */

    /* / */
    begin_node(&f, "");
    prop_u32(&f, "#address-cells", 2);
    prop_u32(&f, "#size-cells", 2);
    prop_str(&f, "compatible", "quanta,virt");
    prop_str(&f, "model", "quanta,virt");

    /* /chosen — where the kernel finds its console and command line. */
    begin_node(&f, "chosen");
    prop_str(&f, "stdout-path", "/soc/uart@10000000");
    prop_str(&f, "bootargs", cfg->bootargs ? cfg->bootargs : "");
    /* An initramfs the firmware staged in RAM: point the kernel at its physical
     * bounds. Encoded as two big-endian cells (a 64-bit value with a zero high
     * cell) — the kernel reads whatever cell count the property length implies. */
    if (cfg->initrd_end > cfg->initrd_start) {
        uint32_t start[2] = { 0, cfg->initrd_start };
        uint32_t end[2]   = { 0, cfg->initrd_end };
        prop_cells(&f, "linux,initrd-start", start, 2);
        prop_cells(&f, "linux,initrd-end", end, 2);
    }
    end_node(&f);

    /* /cpus — one node per hart, each with its local interrupt controller. */
    uint32_t nharts = cfg->nharts ? cfg->nharts : 1;
    begin_node(&f, "cpus");
    prop_u32(&f, "#address-cells", 1);
    prop_u32(&f, "#size-cells", 0);
    prop_u32(&f, "timebase-frequency", cfg->timebase_freq);
    for (uint32_t h = 0; h < nharts; h++) {
        snprintf(unit, sizeof unit, "cpu@%x", h);
        begin_node(&f, unit);
        prop_str(&f, "device_type", "cpu");
        prop_u32(&f, "reg", h);
        prop_str(&f, "status", "okay");
        prop_str(&f, "compatible", "riscv");
        prop_str(&f, "riscv,isa", cfg->isa);
        prop_str(&f, "mmu-type", cfg->mmu_type);
        begin_node(&f, "interrupt-controller");
        prop_u32(&f, "#interrupt-cells", 1);
        prop_empty(&f, "interrupt-controller");
        prop_str(&f, "compatible", "riscv,cpu-intc");
        prop_u32(&f, "phandle", PHANDLE_CPU_INTC(h));
        end_node(&f);  /* interrupt-controller */
        end_node(&f);  /* cpu@h */
    }
    end_node(&f);  /* cpus */

    /* /memory@<base> — the RAM the loader gave this machine. */
    snprintf(unit, sizeof unit, "memory@%x", cfg->mem_base);
    begin_node(&f, unit);
    prop_str(&f, "device_type", "memory");
    prop_reg(&f, cfg->mem_base, cfg->mem_size);
    end_node(&f);

    /* /soc — the memory-mapped devices (M13), on the qemu 'virt' map. */
    begin_node(&f, "soc");
    prop_u32(&f, "#address-cells", 2);
    prop_u32(&f, "#size-cells", 2);
    prop_str(&f, "compatible", "simple-bus");
    prop_empty(&f, "ranges");

    /* Each hart contributes a (phandle, cause) pair to the interrupt controllers'
     * `interrupts-extended`: the CLINT wires each hart's software (3) and timer
     * (7) lines, the PLIC each hart's machine-external (11) and supervisor-external
     * (9) lines. Sized for the worst case (all harts) — well under DTB_MAX_SIZE. */
    uint32_t irqs[4 * QUANTA_MAX_HARTS];

    /* CLINT: machine software (IPI, irq 3) and timer (irq 7) into each hart. */
    snprintf(unit, sizeof unit, "clint@%x", cfg->clint_base);
    begin_node(&f, unit);
    prop_str(&f, "compatible", "riscv,clint0");
    prop_reg(&f, cfg->clint_base, cfg->clint_size);
    for (uint32_t h = 0; h < nharts; h++) {
        irqs[4 * h + 0] = PHANDLE_CPU_INTC(h); irqs[4 * h + 1] = 3;
        irqs[4 * h + 2] = PHANDLE_CPU_INTC(h); irqs[4 * h + 3] = 7;
    }
    prop_cells(&f, "interrupts-extended", irqs, 4 * nharts);
    end_node(&f);

    /* PLIC: external interrupts, machine (irq 11) and supervisor (irq 9) per hart. */
    snprintf(unit, sizeof unit, "plic@%x", cfg->plic_base);
    begin_node(&f, unit);
    prop_str(&f, "compatible", "riscv,plic0");
    prop_reg(&f, cfg->plic_base, cfg->plic_size);
    prop_u32(&f, "#interrupt-cells", 1);
    prop_empty(&f, "interrupt-controller");
    for (uint32_t h = 0; h < nharts; h++) {
        irqs[4 * h + 0] = PHANDLE_CPU_INTC(h); irqs[4 * h + 1] = 11;
        irqs[4 * h + 2] = PHANDLE_CPU_INTC(h); irqs[4 * h + 3] = 9;
    }
    prop_cells(&f, "interrupts-extended", irqs, 4 * nharts);
    prop_u32(&f, "riscv,ndev", cfg->plic_ndev);
    prop_u32(&f, "phandle", PHANDLE_PLIC);
    end_node(&f);

    /* UART: a 16550, its interrupt routed to the PLIC. */
    snprintf(unit, sizeof unit, "uart@%x", cfg->uart_base);
    begin_node(&f, unit);
    prop_str(&f, "compatible", "ns16550a");
    prop_reg(&f, cfg->uart_base, cfg->uart_size);
    prop_u32(&f, "clock-frequency", 3686400);
    prop_u32(&f, "interrupt-parent", PHANDLE_PLIC);
    prop_u32(&f, "interrupts", cfg->uart_irq);
    end_node(&f);

    /* virtio-mmio network device (M23): advertised only when a --netdev backend
     * is attached (base != 0), so a guest OS binds it exactly when it is usable.
     * Its interrupt is routed to the PLIC like the UART's. */
    if (cfg->virtio_net_base) {
        snprintf(unit, sizeof unit, "virtio@%x", cfg->virtio_net_base);
        begin_node(&f, unit);
        prop_str(&f, "compatible", "virtio,mmio");
        prop_reg(&f, cfg->virtio_net_base, cfg->virtio_net_size);
        prop_u32(&f, "interrupt-parent", PHANDLE_PLIC);
        prop_u32(&f, "interrupts", cfg->virtio_net_irq);
        end_node(&f);
    }

    /* SiFive test finisher: the firmware's (and OS's) poweroff/reboot device.
     * The compatible is a two-string list — OpenSBI's reset driver binds to
     * either "sifive,test1" or "sifive,test0". */
    snprintf(unit, sizeof unit, "test@%x", cfg->test_base);
    begin_node(&f, unit);
    static const char test_compat[] = "sifive,test1\0sifive,test0";
    prop(&f, "compatible", test_compat, (uint32_t)sizeof test_compat);
    prop_reg(&f, cfg->test_base, cfg->test_size);
    end_node(&f);

    end_node(&f);  /* soc */
    end_node(&f);  /* / */
    put_u32(&f, FDT_END);

    if (!f.ok) return 0;

    /* Append the strings block right after the struct block, then we know every
     * size and can back-fill the header. */
    size_t struct_size = f.spos - STRUCT_OFF;
    size_t strings_off = f.spos;
    if (strings_off + f.slen > cap) return 0;
    memcpy(buf + strings_off, f.strings, f.slen);
    size_t total = strings_off + f.slen;

    wr_be(buf + 0,  FDT_MAGIC);
    wr_be(buf + 4,  (uint32_t)total);          /* totalsize        */
    wr_be(buf + 8,  STRUCT_OFF);               /* off_dt_struct    */
    wr_be(buf + 12, (uint32_t)strings_off);    /* off_dt_strings   */
    wr_be(buf + 16, HEADER_SIZE);              /* off_mem_rsvmap   */
    wr_be(buf + 20, FDT_VERSION);
    wr_be(buf + 24, FDT_LAST_COMP);
    wr_be(buf + 28, cfg->boot_hart);           /* boot_cpuid_phys  */
    wr_be(buf + 32, (uint32_t)f.slen);         /* size_dt_strings  */
    wr_be(buf + 36, (uint32_t)struct_size);    /* size_dt_struct   */

    return total;
}
