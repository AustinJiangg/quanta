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
    return in_window(addr, TEST_BASE,   TEST_SIZE)   ||
           in_window(addr, CLINT_BASE,  CLINT_SIZE)  ||
           in_window(addr, PLIC_BASE,   PLIC_SIZE)   ||
           in_window(addr, UART_BASE,   UART_SIZE)   ||
           in_window(addr, VIRTIO_BASE, VIRTIO_SIZE);
}

/* SiFive test finisher — qemu virt's poweroff/reboot device. A 32-bit write
 * whose low half is FINISHER_PASS/FAIL/RESET ends the machine; FAIL carries an
 * exit code in the high half. We cannot reboot, so RESET halts like PASS. The
 * effect is deferred to plat_poweroff_requested, which the CPU polls. */
#define FINISHER_FAIL  0x3333u
#define FINISHER_PASS  0x5555u
#define FINISHER_RESET 0x7777u

static void test_write(Platform *p, uint32_t value) {
    switch (value & 0xffffu) {
        case FINISHER_FAIL:
            p->poweroff = 1; p->poweroff_code = (value >> 16) & 0xffu; break;
        case FINISHER_PASS:
        case FINISHER_RESET:
            p->poweroff = 1; p->poweroff_code = 0; break;
        default: break;
    }
}

int plat_poweroff_requested(const Platform *p, uint32_t *code) {
    if (!p->poweroff) return 0;
    if (code) *code = p->poweroff_code;
    return 1;
}

void plat_attach_ram(Platform *p, uint8_t *ram, uint64_t base, uint64_t size) {
    p->ram      = ram;
    p->ram_base = base;
    p->ram_size = size;
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

/* Is the UART asserting its interrupt line? Received-data (IER bit 0) when a byte
 * waits, or transmit-holding-empty (IER bit 1) while a THRE interrupt is pending.
 * THRE is a one-shot (thre_ip), set when THR is written or the TX interrupt is
 * enabled and cleared by reading IIR — so an always-empty transmitter does not
 * assert the line forever (which would storm an OS that leaves TX ints enabled). */
static int uart_asserted(const Uart *u) {
    return ((u->ier & 0x01u) && u->rx_have) || ((u->ier & 0x02u) && u->thre_ip);
}

/* Interrupt-identification register: RX outranks THRE; bit 0 set means "no
 * interrupt pending". */
static uint8_t uart_iir(const Uart *u) {
    if ((u->ier & 0x01u) && u->rx_have)  return 0x04u; /* received data available */
    if ((u->ier & 0x02u) && u->thre_ip)  return 0x02u; /* THR empty */
    return 0x01u;                                      /* none pending */
}

static uint32_t uart_read(Uart *u, uint32_t off) {
    int dlab = (u->lcr & 0x80u) != 0;
    switch (off) {
        case 0: /* RBR (or DLL when DLAB) */
            if (dlab) return u->dll;
            u->rx_have = 0;
            return u->rx;
        case 1: return dlab ? u->dlm : u->ier;
        case 2: {
            uint8_t iir = uart_iir(u);
            if (iir == 0x02u) u->thre_ip = 0; /* reading IIR clears a THRE interrupt */
            return iir;
        }
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
            u->thre_ip = 1;             /* THR emptied (instantly): re-arm THRE int */
            return;
        case 1:
            if (dlab) { u->dlm = val; return; }
            u->ier = (uint8_t)(val & 0x0fu);
            if (u->ier & 0x02u) u->thre_ip = 1; /* enabling TX int with THR empty asserts */
            return;
        case 2: return;                 /* FCR (FIFO control): accepted, ignored */
        case 3: u->lcr = val; return;
        case 4: u->mcr = val; return;
        case 7: u->scr = val; return;
        default: return;
    }
}

/* Buffer a received byte for the guest. The interrupt follows automatically:
 * uart_asserted() sees rx_have with IER's RX bit set, so plic_lines() raises the
 * UART source and plat_mip_bits() reports MEIP on the next pull. A full buffer
 * rejects the byte (0) so the caller can hold and retry rather than lose input. */
int plat_uart_rx(Platform *p, uint8_t byte) {
    if (p->uart.rx_have) return 0;
    p->uart.rx = byte;
    p->uart.rx_have = 1;
    return 1;
}

/* ------------------------------------------------------------------------
 * virtio-mmio block device (modern / version 2).
 *
 * A single split virtqueue serves the attached --disk image as a block device.
 * The driver (e.g. xv6) builds the descriptor table, available ring, and used
 * ring in guest RAM, then kicks the device by writing QUEUE_NOTIFY. We service
 * every newly-available descriptor chain synchronously: read the block request
 * header, DMA sectors between the disk image and the guest buffers, write a
 * status byte, mark the request complete in the used ring, and raise the PLIC
 * interrupt. Doing the work on the notify (rather than on a background thread)
 * keeps the model deterministic and is safe for xv6, which holds its disk lock
 * with interrupts off until it sleeps waiting for completion.
 * ------------------------------------------------------------------------ */

/* Virtio-mmio register offsets — the subset a block driver touches. */
#define V_MAGIC          0x000  /* "virt" */
#define V_VERSION        0x004  /* 2 = modern */
#define V_DEVICE_ID      0x008  /* 2 = block */
#define V_VENDOR_ID      0x00c  /* "QEMU" */
#define V_DEVICE_FEAT    0x010
#define V_DEVICE_FEAT_SEL 0x014
#define V_DRIVER_FEAT    0x020
#define V_DRIVER_FEAT_SEL 0x024
#define V_QUEUE_SEL      0x030
#define V_QUEUE_NUM_MAX  0x034
#define V_QUEUE_NUM      0x038
#define V_QUEUE_READY    0x044
#define V_QUEUE_NOTIFY   0x050
#define V_INT_STATUS     0x060
#define V_INT_ACK        0x064
#define V_STATUS         0x070
#define V_DESC_LOW       0x080
#define V_DESC_HIGH      0x084
#define V_AVAIL_LOW      0x090
#define V_AVAIL_HIGH     0x094
#define V_USED_LOW       0x0a0
#define V_USED_HIGH      0x0a4
#define V_CONFIG_GEN     0x0fc
#define V_CONFIG         0x100  /* device config space (block: capacity at +0) */

#define V_MAGIC_VALUE  0x74726976u  /* "virt" */
#define V_VENDOR_QEMU  0x554d4551u  /* "QEMU" */
#define V_BLK_ID       2u
#define V_QUEUE_MAX    8u           /* ring entries we support (xv6 uses 8) */

#define VRING_DESC_F_NEXT  1u       /* buffer continues in `next` */
#define VRING_DESC_F_WRITE 2u       /* device-writable (vs device-readable) */

#define VIRTIO_BLK_T_IN  0u         /* read: disk -> memory  */
#define VIRTIO_BLK_T_OUT 1u         /* write: memory -> disk */
#define VIRTIO_BLK_S_OK    0u
#define VIRTIO_BLK_S_IOERR 1u
#define SECTOR_SIZE 512u

/* Bounds-checked pointer into guest RAM for a DMA of `len` bytes at physical
 * address `addr`. NULL means the range is out of RAM (or no RAM is attached): a
 * malformed descriptor is then ignored rather than corrupting the host — the DMA
 * never touches the CPU's fault path. */
static uint8_t *dma_ptr(Platform *p, uint64_t addr, uint64_t len) {
    if (!p->ram || len > p->ram_size || addr < p->ram_base) return NULL;
    uint64_t off = addr - p->ram_base;
    if (off > p->ram_size - len) return NULL;
    return p->ram + off;
}

/* Little-endian load/store helpers over a DMA pointer. */
static uint16_t ld16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }
static uint32_t ld32(const uint8_t *b) {
    return (uint32_t)b[0] | (uint32_t)b[1] << 8
         | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static uint64_t ld64(const uint8_t *b) {
    return (uint64_t)ld32(b) | (uint64_t)ld32(b + 4) << 32;
}
static void st16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void st32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)v;         b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}

/* Service one request chain rooted at descriptor `head`: descriptor 0 is the
 * block request header (type + starting sector), the last descriptor is a 1-byte
 * status the device writes, and the descriptors between are the data payload.
 * Returns the number of bytes written into device-writable buffers (for the used
 * ring's length field), including the status byte. */
static uint32_t virtio_service(Platform *p, uint16_t head) {
    Virtio *v = &p->virtio;
    uint32_t qn = v->queue_num;
    if (qn == 0 || qn > V_QUEUE_MAX) return 0;

    /* Collect the chain, bounded by the ring size so a cyclic `next` can't spin. */
    uint64_t addr[V_QUEUE_MAX];
    uint32_t len[V_QUEUE_MAX];
    uint32_t n = 0;
    uint16_t idx = head;
    for (;;) {
        if (n >= qn) break;
        uint8_t *d = dma_ptr(p, v->desc_addr + (uint64_t)idx * 16, 16);
        if (!d) break;
        addr[n] = ld64(d);
        len[n]  = ld32(d + 8);
        uint16_t flags = ld16(d + 12);
        uint16_t next  = ld16(d + 14);
        n++;
        if (!(flags & VRING_DESC_F_NEXT)) break;
        idx = next;
    }
    if (n < 2) return 0;   /* need at least a header and a status byte */

    /* Descriptor 0: the request header. */
    uint32_t type = VIRTIO_BLK_T_IN;
    uint64_t sector = 0;
    uint8_t *hdr = dma_ptr(p, addr[0], 16);
    if (hdr) { type = ld32(hdr); sector = ld64(hdr + 8); }

    /* Descriptors 1..n-2: the data buffers. Copy each between guest RAM and the
     * disk image, advancing the byte offset. Reads past the image end yield 0;
     * writes past it are dropped — a short/oversized image never overruns. */
    uint32_t last = n - 1;
    uint64_t off = sector * SECTOR_SIZE;
    uint32_t written = 0;
    uint8_t status = VIRTIO_BLK_S_OK;
    for (uint32_t i = 1; i < last; i++) {
        uint8_t *buf = dma_ptr(p, addr[i], len[i]);
        if (!buf) { status = VIRTIO_BLK_S_IOERR; break; }
        for (uint32_t k = 0; k < len[i]; k++) {
            uint64_t pos = off + k;
            int in_image = p->disk.data && pos < p->disk.size;
            if (type == VIRTIO_BLK_T_IN)                    /* disk -> memory */
                buf[k] = in_image ? p->disk.data[pos] : 0;
            else if (type == VIRTIO_BLK_T_OUT && in_image)  /* memory -> disk */
                p->disk.data[pos] = buf[k];
        }
        if (type == VIRTIO_BLK_T_IN) written += len[i];
        off += len[i];
    }

    /* Last descriptor: the status byte the driver reads on completion. */
    uint8_t *st = dma_ptr(p, addr[last], 1);
    if (st) *st = status;

    return written + 1;   /* + the status byte the device wrote */
}

/* Consume every newly-available request and post each to the used ring. */
static void virtio_notify(Platform *p) {
    Virtio *v = &p->virtio;
    if (!v->queue_ready || v->queue_num == 0 || v->queue_num > V_QUEUE_MAX) return;

    uint8_t *avail = dma_ptr(p, v->avail_addr, 4 + (uint64_t)v->queue_num * 2);
    uint8_t *used  = dma_ptr(p, v->used_addr,  4 + (uint64_t)v->queue_num * 8);
    if (!avail || !used) return;

    uint16_t avail_idx = ld16(avail + 2);          /* the driver's producer index */
    while (v->last_avail != avail_idx) {
        uint16_t slot = (uint16_t)(v->last_avail % v->queue_num);
        uint16_t head = ld16(avail + 4 + (size_t)slot * 2);
        uint32_t wlen = virtio_service(p, head);

        uint16_t uidx  = ld16(used + 2);           /* the device's producer index */
        uint8_t *elem  = used + 4 + (size_t)(uidx % v->queue_num) * 8;
        st32(elem, head);                          /* used elem: id = chain head */
        st32(elem + 4, wlen);                       /*            len = bytes written */
        st16(used + 2, (uint16_t)(uidx + 1));       /* publish the completion */
        v->last_avail++;
    }
    v->interrupt_status |= 0x1u;   /* used ring advanced; PLIC IRQ follows via plic_lines */
}

/* A status write of 0 resets the device (virtio's soft reset); the driver does
 * this before re-initialising. Clear all queue/feature/interrupt state. */
static void virtio_reset(Virtio *v) {
    memset(v, 0, sizeof *v);
}

static uint32_t virtio_read(Platform *p, uint32_t off) {
    Virtio *v = &p->virtio;
    switch (off) {
        case V_MAGIC:         return V_MAGIC_VALUE;
        /* version 2 and block-device id 2 are distinct registers, same value. */
        case V_VERSION:       return 2u; // NOLINT(bugprone-branch-clone)
        case V_DEVICE_ID:     return V_BLK_ID;
        case V_VENDOR_ID:     return V_VENDOR_QEMU;
        /* We negotiate only VIRTIO_F_VERSION_1 (feature bit 32), so the low half
         * reads back 0 and the high half reads back bit 0 set. */
        case V_DEVICE_FEAT:   return (v->features_sel == 1) ? 1u : 0u;
        case V_QUEUE_NUM_MAX: return V_QUEUE_MAX;
        case V_QUEUE_READY:   return v->queue_ready;
        case V_INT_STATUS:    return v->interrupt_status;
        case V_STATUS:        return v->status;
        case V_CONFIG_GEN:    return 0u;
        /* Block config: capacity in 512-byte sectors (a 64-bit field). */
        case V_CONFIG:        return (uint32_t)(p->disk.size / SECTOR_SIZE);
        case V_CONFIG + 4:    return (uint32_t)((p->disk.size / SECTOR_SIZE) >> 32);
        default:              return 0u;
    }
}

static void virtio_write(Platform *p, uint32_t off, uint32_t val) {
    Virtio *v = &p->virtio;
    switch (off) {
        case V_DEVICE_FEAT_SEL: v->features_sel = val; return;
        case V_QUEUE_NUM:       v->queue_num = val; return;
        case V_QUEUE_READY:     v->queue_ready = val & 1u; return;
        case V_QUEUE_NOTIFY:    virtio_notify(p); return;
        case V_INT_ACK:         v->interrupt_status &= ~val; return;
        case V_STATUS:
            v->status = val;
            if (val == 0) virtio_reset(v);
            return;
        case V_DESC_LOW:   v->desc_addr  = (v->desc_addr  & 0xffffffff00000000ull) | val; return;
        case V_DESC_HIGH:  v->desc_addr  = (v->desc_addr  & 0x00000000ffffffffull) | ((uint64_t)val << 32); return;
        case V_AVAIL_LOW:  v->avail_addr = (v->avail_addr & 0xffffffff00000000ull) | val; return;
        case V_AVAIL_HIGH: v->avail_addr = (v->avail_addr & 0x00000000ffffffffull) | ((uint64_t)val << 32); return;
        case V_USED_LOW:   v->used_addr  = (v->used_addr  & 0xffffffff00000000ull) | val; return;
        case V_USED_HIGH:  v->used_addr  = (v->used_addr  & 0x00000000ffffffffull) | ((uint64_t)val << 32); return;
        default: return; /* QUEUE_SEL, DRIVER_FEAT(_SEL): accepted, ignored (one queue, no negotiation) */
    }
}

/* ------------------------------------------------------------------------
 * PLIC.
 * ------------------------------------------------------------------------ */

/* Bitmap of sources currently asserting their interrupt line: the UART (source
 * 10) and the virtio block device (source 1). Other sources are always quiet. */
static uint32_t plic_lines(const Platform *p) {
    uint32_t lines = 0;
    if (uart_asserted(&p->uart))    lines |= (1u << UART_IRQ);
    if (p->virtio.interrupt_status) lines |= (1u << VIRTIO_IRQ);
    return lines;
}

/* The highest-priority source that is asserting, enabled for context `ctx`, above
 * that context's threshold, and not already in service there. 0 means nothing to
 * present. Ties break toward the lower source number. Context 0 is hart 0 M-mode
 * (drives MEIP), context 1 is hart 0 S-mode (drives SEIP). */
static uint32_t plic_best(const Platform *p, uint32_t ctx) {
    const Plic *pl = &p->plic;
    uint32_t lines = plic_lines(p);
    uint32_t best = 0, best_prio = 0;
    for (uint32_t s = 1; s < PLIC_NSOURCES; s++) {
        if (!(lines & (1u << s)))                  continue;
        if (!(pl->enable[ctx] & (1u << s)))        continue;
        if (s == pl->claimed[ctx])                 continue;
        if (pl->priority[s] <= pl->threshold[ctx]) continue;
        if (pl->priority[s] > best_prio) { best = s; best_prio = pl->priority[s]; }
    }
    return best;
}

/* Claim the pending interrupt for `ctx`: return its source and mark it in service
 * so it is not presented again until completed. */
static uint32_t plic_claim(Platform *p, uint32_t ctx) {
    uint32_t s = plic_best(p, ctx);
    if (s) p->plic.claimed[ctx] = s;
    return s;
}

/* The per-context enable bitmap lives at 0x2000 + ctx*0x80 (one word covers the
 * 32 sources we model); the threshold/claim pair at 0x200000 + ctx*0x1000
 * (threshold at +0, claim/complete at +4) — the qemu virt PLIC layout. */
static uint32_t plic_read(Platform *p, uint32_t off) {
    if (off < 0x1000u) {                       /* per-source priority */
        uint32_t s = off / 4u;
        return (s < PLIC_NSOURCES) ? p->plic.priority[s] : 0u;
    }
    if (off == 0x1000u) return plic_lines(p);  /* pending bitmap (sources 0..31) */
    if (off >= 0x2000u && off < 0x2000u + PLIC_NCONTEXTS * 0x80u) {
        uint32_t rel = off - 0x2000u;
        return ((rel % 0x80u) == 0) ? p->plic.enable[rel / 0x80u] : 0u;
    }
    if (off >= 0x200000u && off < 0x200000u + PLIC_NCONTEXTS * 0x1000u) {
        uint32_t rel = off - 0x200000u, ctx = rel / 0x1000u, reg = rel % 0x1000u;
        if (reg == 0) return p->plic.threshold[ctx];
        if (reg == 4) return plic_claim(p, ctx); /* claim (read has the side effect) */
    }
    return 0;
}

static void plic_write(Platform *p, uint32_t off, uint32_t val) {
    if (off < 0x1000u) {                        /* per-source priority */
        uint32_t s = off / 4u;
        if (s >= 1u && s < PLIC_NSOURCES) p->plic.priority[s] = val;
        return;
    }
    if (off >= 0x2000u && off < 0x2000u + PLIC_NCONTEXTS * 0x80u) {
        uint32_t rel = off - 0x2000u;
        if ((rel % 0x80u) == 0) p->plic.enable[rel / 0x80u] = val;
        return;
    }
    if (off >= 0x200000u && off < 0x200000u + PLIC_NCONTEXTS * 0x1000u) {
        uint32_t rel = off - 0x200000u, ctx = rel / 0x1000u, reg = rel % 0x1000u;
        if (reg == 0) p->plic.threshold[ctx] = val;
        else if (reg == 4 && val == p->plic.claimed[ctx])
            p->plic.claimed[ctx] = 0;           /* complete: end the claim */
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

    /* CLINT/PLIC/virtio expose word registers; honour a sub-word read by shifting. */
    uint32_t word;
    if (in_window(addr, CLINT_BASE, CLINT_SIZE))
        word = clint_read(&p->clint, (addr - CLINT_BASE) & ~3u);
    else if (in_window(addr, PLIC_BASE, PLIC_SIZE))
        word = plic_read(p, (addr - PLIC_BASE) & ~3u);
    else if (in_window(addr, VIRTIO_BASE, VIRTIO_SIZE))
        word = virtio_read(p, (uint32_t)(addr - VIRTIO_BASE) & ~3u);
    else
        return 0;

    word >>= 8u * (addr & 3u);
    if (size == 1) return word & 0xffu;
    if (size == 2) return word & 0xffffu;
    return word;
}

void plat_write(Platform *p, uint64_t addr, uint32_t size, uint32_t value) {
    (void)size; /* CLINT/PLIC/virtio take word writes; the UART is byte-addressed */
    if (in_window(addr, TEST_BASE, TEST_SIZE))
        test_write(p, value);
    else if (in_window(addr, UART_BASE, UART_SIZE))
        uart_write(&p->uart, addr - UART_BASE, (uint8_t)value);
    else if (in_window(addr, CLINT_BASE, CLINT_SIZE))
        clint_write(&p->clint, (addr - CLINT_BASE) & ~3u, value);
    else if (in_window(addr, PLIC_BASE, PLIC_SIZE))
        plic_write(p, (addr - PLIC_BASE) & ~3u, value);
    else if (in_window(addr, VIRTIO_BASE, VIRTIO_SIZE))
        virtio_write(p, (uint32_t)(addr - VIRTIO_BASE) & ~3u, value);
}

uint32_t plat_mip_bits(Platform *p) {
    uint32_t bits = 0;
    if (p->clint.mtime >= p->clint.mtimecmp) bits |= MIP_MTIP;
    if (p->clint.msip & 1u)                  bits |= MIP_MSIP;
    if (plic_best(p, 0) != 0)                bits |= MIP_MEIP; /* M-mode context */
    if (plic_best(p, 1) != 0)                bits |= MIP_SEIP; /* S-mode context */
    return bits;
}
