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
 *
 * The models are self-contained (no CPU or memory dependency): the memory layer
 * dispatches MMIO accesses here, and the CPU pulls the resulting interrupt-
 * pending bits each step through plat_mip_bits(). The addresses follow the
 * de-facto qemu 'virt' layout so guest software and device trees line up.
 */

#define CLINT_BASE 0x02000000u
#define CLINT_SIZE 0x00010000u
#define PLIC_BASE  0x0c000000u
#define PLIC_SIZE  0x04000000u
#define UART_BASE  0x10000000u
#define UART_SIZE  0x00000100u

/* A handful of PLIC sources is plenty; the UART is wired to source 10, as on
 * qemu virt. One hart, one interrupt context (hart 0, M-mode). */
#define PLIC_NSOURCES 32u
#define UART_IRQ      10u

/* mip/mie bit positions the platform drives — read-only reflections of device
 * state from software's point of view (machine software/timer/external). */
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
} Uart;

typedef struct {
    uint32_t priority[PLIC_NSOURCES]; /* per-source priority (0 disables) */
    uint32_t enable;                  /* context-0 source-enable bitmap */
    uint32_t threshold;               /* context-0 priority threshold */
    uint32_t claimed;                 /* source currently in service (0 = none) */
} Plic;

/* A raw block-device backing image (attached via --disk). Held here so a future
 * virtio-mmio block device can DMA against it; loaded into RAM so reads and
 * writes hit the buffer (writes do not persist to the file). data == NULL when
 * no disk is attached. The bytes are owned by the engine, which frees them. */
typedef struct {
    uint8_t *data;
    uint64_t size;
} Disk;

typedef struct Platform {
    Clint clint;
    Uart  uart;
    Plic  plic;
    Disk  disk;
} Platform;

/* Reset all devices: zeroed register files, mtimecmp parked at all-ones so the
 * timer stays quiet until the guest programs it. */
void plat_init(Platform *p);

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

#endif /* QUANTA_DEVICE_H */
