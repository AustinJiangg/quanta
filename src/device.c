#include "device.h"

#include <stdio.h>
#include <string.h>

/*
 * MMIO device models — see device.h for the platform overview. Each device is a
 * small register file with read/write side effects; the memory layer routes
 * MMIO addresses here, and the CPU pulls plat_mip_bits() each step to learn
 * which interrupts the devices are asserting.
 */

/* True when `addr` lies in [base, base+size). Written to stay correct for
 * addresses below base (the unsigned difference wraps high, failing the test). */
static int in_window(uint64_t addr, uint32_t base, uint32_t size) {
    return (addr - base) < size;
}

void plat_init(Platform *p) {
    memset(p, 0, sizeof *p);
    p->clint.mtimecmp = (uint64_t)-1; /* parked: no timer interrupt until armed */
}

int plat_contains(uint64_t addr) {
    return in_window(addr, CLINT_BASE, CLINT_SIZE) ||
           in_window(addr, PLIC_BASE,  PLIC_SIZE)  ||
           in_window(addr, UART_BASE,  UART_SIZE);
}

void plat_tick(Platform *p) {
    p->clint.mtime++;
}

/* ------------------------------------------------------------------------
 * 16550 UART.
 * ------------------------------------------------------------------------ */

/* The line-status register: transmitter always ready (THR/TEMT empty, since we
 * print synchronously), data-ready when a receive byte is buffered. */
static uint8_t uart_lsr(const Uart *u) {
    return (uint8_t)(0x60u | (u->rx_have ? 0x01u : 0x00u));
}

/* Is the UART asserting its interrupt line? Received-data (IER bit 0) when a
 * byte waits, or transmit-holding-empty (IER bit 1) — which, as our transmitter
 * is always empty, stays asserted until software clears that enable. */
static int uart_asserted(const Uart *u) {
    return ((u->ier & 0x01u) && u->rx_have) || ((u->ier & 0x02u) != 0);
}

/* Interrupt-identification register: RX outranks THRE; bit 0 set means "no
 * interrupt pending". */
static uint8_t uart_iir(const Uart *u) {
    if ((u->ier & 0x01u) && u->rx_have) return 0x04u; /* received data available */
    if (u->ier & 0x02u)                 return 0x02u; /* THR empty */
    return 0x01u;                                     /* none pending */
}

static uint32_t uart_read(Uart *u, uint32_t off) {
    int dlab = (u->lcr & 0x80u) != 0;
    switch (off) {
        case 0: /* RBR (or DLL when DLAB) */
            if (dlab) return u->dll;
            u->rx_have = 0;
            return u->rx;
        case 1: return dlab ? u->dlm : u->ier;
        case 2: return uart_iir(u);
        case 3: return u->lcr;
        case 4: return u->mcr;
        case 5: return uart_lsr(u);
        case 6: return 0x00u;          /* modem status: nothing connected */
        case 7: return u->scr;
        default: return 0;
    }
}

static void uart_write(Uart *u, uint32_t off, uint8_t val) {
    int dlab = (u->lcr & 0x80u) != 0;
    switch (off) {
        case 0:
            if (dlab) { u->dll = val; return; }
            putchar((int)val);          /* THR: transmit to the host console */
            fflush(stdout);
            return;
        case 1: if (dlab) u->dlm = val; else u->ier = (uint8_t)(val & 0x0fu); return;
        case 2: return;                 /* FCR (FIFO control): accepted, ignored */
        case 3: u->lcr = val; return;
        case 4: u->mcr = val; return;
        case 7: u->scr = val; return;
        default: return;
    }
}

/* ------------------------------------------------------------------------
 * PLIC.
 * ------------------------------------------------------------------------ */

/* Bitmap of sources currently asserting their interrupt line. Only the UART is
 * wired up; other sources are always quiet. */
static uint32_t plic_lines(const Platform *p) {
    return uart_asserted(&p->uart) ? (1u << UART_IRQ) : 0u;
}

/* The highest-priority source that is asserting, enabled, above the threshold,
 * and not already in service. 0 means nothing to present. Ties break toward the
 * lower source number. */
static uint32_t plic_best(const Platform *p) {
    const Plic *pl = &p->plic;
    uint32_t lines = plic_lines(p);
    uint32_t best = 0, best_prio = 0;
    for (uint32_t s = 1; s < PLIC_NSOURCES; s++) {
        if (!(lines & (1u << s)))        continue;
        if (!(pl->enable & (1u << s)))   continue;
        if (s == pl->claimed)            continue;
        if (pl->priority[s] <= pl->threshold) continue;
        if (pl->priority[s] > best_prio) { best = s; best_prio = pl->priority[s]; }
    }
    return best;
}

/* Claim the pending interrupt: return its source and mark it in service so it is
 * not presented again until completed. */
static uint32_t plic_claim(Platform *p) {
    uint32_t s = plic_best(p);
    if (s) p->plic.claimed = s;
    return s;
}

static uint32_t plic_read(Platform *p, uint32_t off) {
    if (off < 0x1000u) {                       /* per-source priority */
        uint32_t s = off / 4u;
        return (s < PLIC_NSOURCES) ? p->plic.priority[s] : 0u;
    }
    switch (off) {
        case 0x1000: return plic_lines(p);     /* pending bitmap (sources 0..31) */
        case 0x2000: return p->plic.enable;    /* context-0 enables */
        case 0x200000: return p->plic.threshold;
        case 0x200004: return plic_claim(p);   /* claim (read has the side effect) */
        default: return 0;
    }
}

static void plic_write(Platform *p, uint32_t off, uint32_t val) {
    if (off < 0x1000u) {                        /* per-source priority */
        uint32_t s = off / 4u;
        if (s >= 1u && s < PLIC_NSOURCES) p->plic.priority[s] = val;
        return;
    }
    switch (off) {
        case 0x2000:   p->plic.enable = val; return;
        case 0x200000: p->plic.threshold = val; return;
        case 0x200004:                          /* complete: end the claim */
            if (val == p->plic.claimed) p->plic.claimed = 0;
            return;
        default: return;
    }
}

/* ------------------------------------------------------------------------
 * CLINT.
 * ------------------------------------------------------------------------ */

static uint32_t clint_read(Clint *c, uint32_t off) {
    switch (off) {
        case 0x0000: return c->msip & 1u;
        case 0x4000: return (uint32_t)c->mtimecmp;
        case 0x4004: return (uint32_t)(c->mtimecmp >> 32);
        case 0xbff8: return (uint32_t)c->mtime;
        case 0xbffc: return (uint32_t)(c->mtime >> 32);
        default: return 0;
    }
}

static void clint_write(Clint *c, uint32_t off, uint32_t val) {
    switch (off) {
        case 0x0000: c->msip = val & 1u; return;
        case 0x4000: c->mtimecmp = (c->mtimecmp & 0xffffffff00000000ull) | val; return;
        case 0x4004: c->mtimecmp = (c->mtimecmp & 0x00000000ffffffffull)
                                 | ((uint64_t)val << 32); return;
        case 0xbff8: c->mtime = (c->mtime & 0xffffffff00000000ull) | val; return;
        case 0xbffc: c->mtime = (c->mtime & 0x00000000ffffffffull)
                              | ((uint64_t)val << 32); return;
        default: return;
    }
}

/* ------------------------------------------------------------------------
 * MMIO dispatch and interrupt pull.
 * ------------------------------------------------------------------------ */

uint32_t plat_read(Platform *p, uint64_t addr, uint32_t size) {
    if (in_window(addr, UART_BASE, UART_SIZE)) /* byte registers */
        return uart_read(&p->uart, addr - UART_BASE);

    /* CLINT/PLIC expose word registers; honour a sub-word read by shifting. */
    uint32_t word;
    if (in_window(addr, CLINT_BASE, CLINT_SIZE))
        word = clint_read(&p->clint, (addr - CLINT_BASE) & ~3u);
    else if (in_window(addr, PLIC_BASE, PLIC_SIZE))
        word = plic_read(p, (addr - PLIC_BASE) & ~3u);
    else
        return 0;

    word >>= 8u * (addr & 3u);
    if (size == 1) return word & 0xffu;
    if (size == 2) return word & 0xffffu;
    return word;
}

void plat_write(Platform *p, uint64_t addr, uint32_t size, uint32_t value) {
    (void)size; /* CLINT/PLIC take word writes; the UART is byte-addressed */
    if (in_window(addr, UART_BASE, UART_SIZE))
        uart_write(&p->uart, addr - UART_BASE, (uint8_t)value);
    else if (in_window(addr, CLINT_BASE, CLINT_SIZE))
        clint_write(&p->clint, (addr - CLINT_BASE) & ~3u, value);
    else if (in_window(addr, PLIC_BASE, PLIC_SIZE))
        plic_write(p, (addr - PLIC_BASE) & ~3u, value);
}

uint32_t plat_mip_bits(Platform *p) {
    uint32_t bits = 0;
    if (p->clint.mtime >= p->clint.mtimecmp) bits |= MIP_MTIP;
    if (p->clint.msip & 1u)                  bits |= MIP_MSIP;
    if (plic_best(p) != 0)                   bits |= MIP_MEIP;
    return bits;
}
