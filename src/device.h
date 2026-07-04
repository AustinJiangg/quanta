#ifndef QUANTA_DEVICE_H
#define QUANTA_DEVICE_H

#include <stdint.h>

/*
 * Platform devices (M13): the memory-mapped hardware a full system needs beyond
 * RAM. Three models live here, each a small register file reached through MMIO:
 *
 *   - CLINT  — the core-local interruptor: a free-running timer (mtime) with a
 *     per-hart compare (mtimecmp) that drives the machine timer interrupt, and a
 *     software-interrupt register (msip) for IPIs.
 *   - PLIC   — the platform-level interrupt controller: routes external device
 *     interrupts to the hart with per-source priority, an enable bitmap, a
 *     threshold, and a claim/complete handshake.
 *   - UART   — a 16550 serial port; writes to its transmit register print to the
 *     host stdout, and it can raise an external interrupt through the PLIC.
 *   - VIRTIO — a virtio-mmio block device (modern / v2) serving the --disk image
 *     over a single split virtqueue; the OS's root filesystem lives on it. Unlike
 *     the others it is a bus master — it DMAs against guest RAM — so the platform
 *     carries a pointer to memory for it (plat_attach_ram).
 *
 * The register models are self-contained (no CPU dependency): the memory layer
 * dispatches MMIO accesses here, and the CPU pulls the resulting interrupt-
 * pending bits each step through plat_mip_bits(). The addresses follow the
 * de-facto qemu 'virt' layout so guest software and device trees line up.
 */

#define TEST_BASE   0x00100000u   /* SiFive test finisher (qemu virt poweroff/reboot) */
#define TEST_SIZE   0x00001000u
#define CLINT_BASE  0x02000000u
#define CLINT_SIZE  0x00010000u
#define PLIC_BASE   0x0c000000u
#define PLIC_SIZE   0x04000000u
#define UART_BASE   0x10000000u
#define UART_SIZE   0x00000100u
#define VIRTIO_BASE 0x10001000u   /* qemu virt's first virtio-mmio slot (xv6's VIRTIO0) */
#define VIRTIO_SIZE 0x00001000u

/* A handful of PLIC sources is plenty; the UART is wired to source 10 and the
 * virtio block device to source 1, as on qemu virt. One hart, but two interrupt
 * contexts: context 0 = hart 0 M-mode (drives MEIP), context 1 = hart 0 S-mode
 * (drives SEIP). An OS like xv6 runs in S-mode and uses context 1. */
#define PLIC_NSOURCES  32u
#define PLIC_NCONTEXTS 2u
#define UART_IRQ       10u
#define VIRTIO_IRQ     1u

/* mip/mie bit positions the platform drives — read-only reflections of device
 * state from software's point of view (machine/supervisor software/timer/
 * external). The PLIC's M-mode context drives MEIP, its S-mode context SEIP. */
#define MIP_SEIP (1u << 9)
#define MIP_MSIP (1u << 3)
#define MIP_MTIP (1u << 7)
#define MIP_MEIP (1u << 11)

typedef struct {
    uint64_t mtime;     /* free-running counter; one tick per retired-or-trapped step */
    uint64_t mtimecmp;  /* hart 0 compare; MTIP asserted while mtime >= mtimecmp */
    uint32_t msip;      /* hart 0 software interrupt pending (bit 0 only) */
} Clint;

typedef struct {
    uint8_t ier;     /* interrupt enable: bit0 = RX data, bit1 = TX holding empty */
    uint8_t lcr;     /* line control: bit7 = DLAB (divisor-latch access) */
    uint8_t mcr;     /* modem control */
    uint8_t scr;     /* scratch register */
    uint8_t dll;     /* divisor latch low  (baud; ignored) */
    uint8_t dlm;     /* divisor latch high (baud; ignored) */
    int     rx_have; /* a byte is buffered for receive */
    uint8_t rx;      /* the buffered receive byte */
    uint8_t thre_ip; /* THR-empty interrupt pending (set on THR write / TX-int enable,
                      * cleared by reading IIR) — so it does not assert forever */
} Uart;

typedef struct {
    uint32_t priority[PLIC_NSOURCES];      /* per-source priority (0 disables) */
    uint32_t enable[PLIC_NCONTEXTS];       /* per-context source-enable bitmap */
    uint32_t threshold[PLIC_NCONTEXTS];    /* per-context priority threshold */
    uint32_t claimed[PLIC_NCONTEXTS];      /* source in service per context (0 = none) */
} Plic;

/* A virtio-mmio block device (modern / version 2) with one split virtqueue. The
 * driver builds the descriptor table, available (driver) ring, and used (device)
 * ring in guest RAM and kicks the device by writing QUEUE_NOTIFY; the device
 * processes each request synchronously, DMAing sectors between the guest buffers
 * and the attached disk image, posts completions to the used ring, and raises
 * PLIC source 1. Only queue 0 exists, and no features are negotiated. */
typedef struct {
    uint32_t status;           /* device status (ACKNOWLEDGE/DRIVER/FEATURES_OK/DRIVER_OK) */
    uint32_t features_sel;     /* DEVICE_FEATURES_SEL: which 32-bit half to read back */
    uint32_t queue_num;        /* negotiated ring size (entries) */
    uint32_t queue_ready;      /* queue 0 live */
    uint64_t desc_addr;        /* guest-physical descriptor table */
    uint64_t avail_addr;       /* guest-physical available (driver) ring */
    uint64_t used_addr;        /* guest-physical used (device) ring */
    uint16_t last_avail;       /* next available index the device will consume */
    uint32_t interrupt_status; /* pending interrupt bits (bit 0 = used ring advanced) */
} Virtio;

/* A raw block-device backing image (attached via --disk). Held here so a future
 * virtio-mmio block device can DMA against it; loaded into RAM so reads and
 * writes hit the buffer (writes do not persist to the file). data == NULL when
 * no disk is attached. The bytes are owned by the engine, which frees them. */
typedef struct {
    uint8_t *data;
    uint64_t size;
} Disk;

typedef struct Platform {
    Clint   clint;
    Uart    uart;
    Plic    plic;
    Virtio  virtio;
    Disk    disk;
    /* Guest RAM, for virtio DMA (the block device is a bus master). Set by
     * plat_attach_ram after the loader initialises memory; NULL disables DMA. */
    uint8_t *ram;
    uint64_t ram_base;
    uint64_t ram_size;
    /* SiFive test finisher: a write of FINISHER_PASS/FAIL/RESET requests machine
     * power-off. The CPU polls plat_poweroff_requested each step and halts, since
     * a device model cannot stop the hart itself. This is how OpenSBI's SRST and
     * Linux's poweroff/reboot actually end a firmware boot. */
    int      poweroff;      /* a power-off has been requested */
    uint32_t poweroff_code; /* exit status to report (0 on PASS/RESET) */
} Platform;

/* Reset all devices: zeroed register files, mtimecmp parked at all-ones so the
 * timer stays quiet until the guest programs it. Clears the RAM pointer too, so
 * call plat_attach_ram afterwards to (re)enable virtio DMA. */
void plat_init(Platform *p);

/* Point the platform at the guest RAM array so the virtio block device can DMA
 * against it: `ram[0]` maps to physical address `base`, spanning `size` bytes. */
void plat_attach_ram(Platform *p, uint8_t *ram, uint64_t base, uint64_t size);

/* Does `addr` fall in any device's MMIO window? The memory layer checks this
 * before its RAM range so device accesses are routed here. */
int plat_contains(uint64_t addr);

/* MMIO access of `size` bytes (1/2/4). Reads return the value (0 for holes);
 * writes commit register effects (a UART transmit prints, a CLINT compare write
 * re-arms the timer, a PLIC complete ends a claim). The address is 64-bit so a
 * high RV64 physical address never false-matches a sub-4 GiB device window. */
uint32_t plat_read(Platform *p, uint64_t addr, uint32_t size);
void     plat_write(Platform *p, uint64_t addr, uint32_t size, uint32_t value);

/* Advance the timer by one tick (called once per CPU step). */
void plat_tick(Platform *p);

/* Deliver a received byte to the UART (as if it arrived on the serial line):
 * buffer it for the guest to read from RBR and, if RX interrupts are enabled,
 * raise the UART's interrupt through the PLIC. Returns 1 if accepted, or 0 when
 * the one-byte receive buffer is still full (the caller should hold the byte and
 * retry). This is the host-input side of the console — the CLI feeds stdin here. */
int plat_uart_rx(Platform *p, uint8_t byte);

/* The interrupt-pending bits the platform drives (MTIP/MSIP/MEIP), to be merged
 * into mip. The CPU pulls this each step rather than the devices pushing it. */
uint32_t plat_mip_bits(Platform *p);

/* Has the SiFive test device been written to request power-off? If so, returns 1
 * and stores the exit status in *code; the CPU then halts the machine (a device
 * cannot stop the hart itself). The CPU polls this once per step. */
int plat_poweroff_requested(const Platform *p, uint32_t *code);

#endif /* QUANTA_DEVICE_H */
