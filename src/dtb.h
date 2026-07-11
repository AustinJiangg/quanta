#ifndef QUANTA_DTB_H
#define QUANTA_DTB_H

#include <stdint.h>
#include <stddef.h>

/* Max harts described (SMP, M19). Canonical definition in quanta.h; guarded here
 * so dtb.c (which includes only this header) can size its interrupt arrays. */
#ifndef QUANTA_MAX_HARTS
#define QUANTA_MAX_HARTS 8u
#endif

/*
 * Flattened device tree (FDT) generation (M14).
 *
 * A booting kernel discovers its hardware not by probing fixed addresses but by
 * reading a *device tree*: a self-describing data structure, handed over at
 * boot, that lists the RAM ranges and the memory-mapped devices and how their
 * interrupts are wired. The RISC-V boot convention passes its physical address
 * in a1 (with the boot hart id in a0); firmware such as OpenSBI or qemu builds
 * the blob before entering the OS. Quanta does the same: it generates a small
 * FDT describing its own RAM and the M13 platform (CLINT, PLIC, 16550 UART),
 * places it in guest memory, and points a1 at it.
 *
 * dtb_build() emits the binary "flattened" form (the .dtb a `dtc` compiler would
 * produce) directly — a big-endian header, a memory-reservation block, a
 * structure block of nested nodes/properties, and a strings block — with no
 * external tooling. It is a pure serialiser: it owns no machine state, just the
 * geometry the caller passes in.
 */

/* The platform geometry the tree describes. Addresses/sizes are physical; the
 * device windows follow the qemu 'virt' layout (see device.h). */
typedef struct {
    uint32_t mem_base;        /* RAM base / size (the /memory node's reg)        */
    uint32_t mem_size;
    uint32_t test_base;       /* SiFive test finisher (poweroff/reboot) window   */
    uint32_t test_size;
    uint32_t clint_base;      /* CLINT, PLIC, UART windows (the /soc children)    */
    uint32_t clint_size;
    uint32_t plic_base;
    uint32_t plic_size;
    uint32_t uart_base;
    uint32_t uart_size;
    uint32_t uart_irq;        /* UART's PLIC source number                       */
    uint32_t plic_ndev;       /* highest PLIC source (riscv,ndev)                 */
    uint32_t boot_hart;       /* boot hart id (a0, and boot_cpuid_phys)          */
    uint32_t nharts;          /* number of harts to describe (>=1; SMP, M19)     */
    uint32_t timebase_freq;   /* /cpus timebase-frequency (informational)        */
    const char *isa;          /* cpu@0 riscv,isa string, e.g. "rv32ima_zicsr"    */
    const char *mmu_type;     /* cpu@0 mmu-type, e.g. "riscv,sv32" / "riscv,none" */
    const char *bootargs;     /* /chosen bootargs (kernel command line); "" if none */
    uint32_t initrd_start;    /* /chosen linux,initrd-start/-end (an initramfs the  */
    uint32_t initrd_end;      /* firmware placed in RAM); both 0 = no initrd        */
    uint32_t virtio_net_base; /* virtio-mmio net device window (0 = not advertised) */
    uint32_t virtio_net_size;
    uint32_t virtio_net_irq;  /* its PLIC source number                            */
} DtbConfig;

/* A comfortable upper bound on the blob this builder emits; a uniprocessor tree
 * is well under 1 KiB, and a full QUANTA_MAX_HARTS SMP tree (one cpu node each)
 * still fits here. Callers can size a stack buffer with this. */
#define DTB_MAX_SIZE 4096u

/* Serialise the device tree for `cfg` into `buf` (capacity `cap`). Returns the
 * number of bytes written, or 0 if anything would not fit (the caller can then
 * fall back to entering the guest without a tree). The result is a complete,
 * standards-conformant FDT a real OS could parse. */
size_t dtb_build(uint8_t *buf, size_t cap, const DtbConfig *cfg);

#endif /* QUANTA_DTB_H */
