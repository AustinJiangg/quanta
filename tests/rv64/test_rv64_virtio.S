/* M18: the virtio-mmio block device (modern / version 2), driven end to end.
 *
 * Acting as a bare-metal (machine-mode) block driver, this test:
 *
 *   1. probes the device identity (magic / version / device-id / vendor),
 *   2. runs the status + feature handshake (reset, ACKNOWLEDGE, DRIVER, no
 *      features, FEATURES_OK) and confirms FEATURES_OK stuck,
 *   3. sets up a single split virtqueue (descriptor table, available ring, used
 *      ring) in RAM and marks the queue ready,
 *   4. reads sector 0 and checks the little-endian magic the harness wrote into
 *      the --disk image (proving disk -> memory DMA of the real backing image),
 *   5. writes a pattern to a sector and reads it back, proving DMA in both
 *      directions through the disk buffer,
 *   6. confirms each notify raised the device's interrupt: the mmio interrupt
 *      status bit and the PLIC pending line for source 1, then acknowledges it.
 *
 * Completion is synchronous (the device services the chain on QUEUE_NOTIFY), so
 * the used ring has already advanced by the time the notify store returns — the
 * test reads the used index directly rather than spinning. It stays in M-mode
 * throughout (Bare translation, so the addresses it hands the device are the
 * physical ones); machine-mode + MMIO + virtio means user-mode qemu can't run
 * it, so it is quanta-only with its own --disk image (make check-virtio), not
 * part of the qemu-riscv64 differential.
 */
#include "test_framework.h"

    /* virtio-mmio register offsets (a block driver's subset). */
    .equ VIRTIO_MAGIC,        0x000
    .equ VIRTIO_VERSION,      0x004
    .equ VIRTIO_DEVICE_ID,    0x008
    .equ VIRTIO_VENDOR,       0x00c
    .equ VIRTIO_DRIVER_FEAT,  0x020
    .equ VIRTIO_QUEUE_SEL,    0x030
    .equ VIRTIO_QUEUE_NUM_MAX,0x034
    .equ VIRTIO_QUEUE_NUM,    0x038
    .equ VIRTIO_QUEUE_READY,  0x044
    .equ VIRTIO_QUEUE_NOTIFY, 0x050
    .equ VIRTIO_INT_STATUS,   0x060
    .equ VIRTIO_INT_ACK,      0x064
    .equ VIRTIO_STATUS,       0x070
    .equ VIRTIO_DESC_LOW,     0x080
    .equ VIRTIO_DESC_HIGH,    0x084
    .equ VIRTIO_AVAIL_LOW,    0x090
    .equ VIRTIO_AVAIL_HIGH,   0x094
    .equ VIRTIO_USED_LOW,     0x0a0
    .equ VIRTIO_USED_HIGH,    0x0a4

    .equ PLIC_PENDING,        0x0c001000   /* pending bitmap, sources 0..31 */

    TEST_START

    li    s0, 0x10001000       /* VIRTIO_BASE */
    la    s1, desc
    la    s2, avail
    la    s3, used
    la    s4, hdr
    la    s5, databuf
    la    s6, statusb

    /* ---- 1. device identity ---- */
    lw    t0, VIRTIO_MAGIC(s0)
    CHECK 1, t0, 0x74726976     /* "virt" */
    lw    t0, VIRTIO_VERSION(s0)
    CHECK 2, t0, 2              /* modern (v2) */
    lw    t0, VIRTIO_DEVICE_ID(s0)
    CHECK 3, t0, 2             /* block device */
    lw    t0, VIRTIO_VENDOR(s0)
    CHECK 4, t0, 0x554d4551    /* "QEMU" */

    /* ---- 2. status handshake + feature negotiation ---- */
    sw    zero, VIRTIO_STATUS(s0)       /* reset */
    li    t0, 1
    sw    t0, VIRTIO_STATUS(s0)         /* ACKNOWLEDGE */
    li    t0, 3
    sw    t0, VIRTIO_STATUS(s0)         /* | DRIVER */
    sw    zero, VIRTIO_DRIVER_FEAT(s0)  /* accept no features */
    li    t0, 0xb
    sw    t0, VIRTIO_STATUS(s0)         /* | FEATURES_OK */
    lw    t0, VIRTIO_STATUS(s0)
    andi  t0, t0, 8
    CHECK 5, t0, 8                      /* FEATURES_OK stuck */

    /* ---- 3. queue 0 setup ---- */
    sw    zero, VIRTIO_QUEUE_SEL(s0)
    lw    t0, VIRTIO_QUEUE_NUM_MAX(s0)
    CHECK 6, t0, 8                      /* advertised ring capacity */
    li    t0, 8
    sw    t0, VIRTIO_QUEUE_NUM(s0)
    sw    s1, VIRTIO_DESC_LOW(s0)       /* descriptor table (guest physical) */
    srli  t0, s1, 32
    sw    t0, VIRTIO_DESC_HIGH(s0)
    sw    s2, VIRTIO_AVAIL_LOW(s0)      /* available (driver) ring */
    srli  t0, s2, 32
    sw    t0, VIRTIO_AVAIL_HIGH(s0)
    sw    s3, VIRTIO_USED_LOW(s0)       /* used (device) ring */
    srli  t0, s3, 32
    sw    t0, VIRTIO_USED_HIGH(s0)
    li    t0, 1
    sw    t0, VIRTIO_QUEUE_READY(s0)
    li    t0, 0xf
    sw    t0, VIRTIO_STATUS(s0)         /* | DRIVER_OK */

    /* ---- build the static 3-descriptor request chain ----
     * desc[0] = header (device-readable) -> desc[1] = data -> desc[2] = status. */
    sd    s4, 0(s1)             /* desc[0].addr = hdr */
    li    t0, 16
    sw    t0, 8(s1)             /* desc[0].len = 16 */
    li    t0, 1
    sh    t0, 12(s1)            /* desc[0].flags = NEXT */
    li    t0, 1
    sh    t0, 14(s1)            /* desc[0].next = 1 */
    sd    s5, 16(s1)            /* desc[1].addr = databuf */
    li    t0, 512
    sw    t0, 24(s1)            /* desc[1].len = 512 */
    li    t0, 2
    sh    t0, 30(s1)            /* desc[1].next = 2 (flags set per request) */
    sd    s6, 32(s1)            /* desc[2].addr = statusb */
    li    t0, 1
    sw    t0, 40(s1)            /* desc[2].len = 1 */
    li    t0, 2
    sh    t0, 44(s1)            /* desc[2].flags = WRITE */
    sh    zero, 46(s1)          /* desc[2].next = 0 */

    /* ---- 4. read sector 0 ---- */
    sw    zero, 0(s4)           /* hdr.type = VIRTIO_BLK_T_IN (read) */
    sw    zero, 4(s4)           /* hdr.reserved */
    sd    zero, 8(s4)           /* hdr.sector = 0 */
    li    t0, 3
    sh    t0, 28(s1)            /* desc[1].flags = NEXT | WRITE (device fills it) */
    sb    zero, 0(s6)           /* clear the status byte */
    sh    zero, 4(s2)           /* avail.ring[0] = head desc 0 */
    li    t0, 1
    sh    t0, 2(s2)             /* avail.idx = 1 */
    sw    zero, VIRTIO_QUEUE_NOTIFY(s0)
    lhu   t0, 2(s3)             /* used.idx advanced synchronously */
    CHECK 7, t0, 1
    lbu   t0, 0(s6)
    CHECK 8, t0, 0             /* status = OK */
    lw    t0, 0(s5)
    CHECK 9, t0, 0x12345678    /* the magic the harness put in sector 0 */

    /* the device raised its interrupt (mmio status + PLIC pending source 1) */
    lw    t0, VIRTIO_INT_STATUS(s0)
    andi  t0, t0, 1
    CHECK 10, t0, 1
    li    t1, PLIC_PENDING
    lw    t0, 0(t1)
    andi  t0, t0, 2            /* 1 << VIRTIO_IRQ */
    CHECK 11, t0, 2
    li    t0, 1
    sw    t0, VIRTIO_INT_ACK(s0)        /* acknowledge -> device deasserts */
    lw    t0, VIRTIO_INT_STATUS(s0)
    CHECK 12, t0, 0

    /* ---- 5a. write a pattern to sector 3 ---- */
    li    t0, 0x5a5a5a5a
    sw    t0, 0(s5)            /* fill the buffer */
    li    t0, 1
    sw    t0, 0(s4)            /* hdr.type = VIRTIO_BLK_T_OUT (write) */
    li    t0, 3
    sd    t0, 8(s4)            /* hdr.sector = 3 */
    li    t0, 1
    sh    t0, 28(s1)           /* desc[1].flags = NEXT (device reads it) */
    sb    zero, 0(s6)
    sh    zero, 6(s2)          /* avail.ring[1] = head desc 0 */
    li    t0, 2
    sh    t0, 2(s2)            /* avail.idx = 2 */
    sw    zero, VIRTIO_QUEUE_NOTIFY(s0)
    lhu   t0, 2(s3)
    CHECK 13, t0, 2
    lbu   t0, 0(s6)
    CHECK 14, t0, 0

    /* ---- 5b. read sector 3 back and confirm the round trip ---- */
    sw    zero, 0(s5)          /* clear the buffer first */
    sw    zero, 0(s4)          /* hdr.type = read */
    li    t0, 3
    sd    t0, 8(s4)            /* hdr.sector = 3 */
    li    t0, 3
    sh    t0, 28(s1)           /* desc[1].flags = NEXT | WRITE */
    sb    zero, 0(s6)
    sh    zero, 8(s2)          /* avail.ring[2] = head desc 0 */
    li    t0, 3
    sh    t0, 2(s2)            /* avail.idx = 3 */
    sw    zero, VIRTIO_QUEUE_NOTIFY(s0)
    lhu   t0, 2(s3)
    CHECK 15, t0, 3
    lbu   t0, 0(s6)
    CHECK 16, t0, 0
    lw    t0, 0(s5)
    CHECK 17, t0, 0x5a5a5a5a   /* the pattern we wrote to sector 3 */

    TEST_END

    .section .bss
    .align 4
desc:    .zero 128    /* 8 descriptors * 16 bytes */
    .align 4
avail:   .zero 32     /* flags, idx, ring[8], used_event */
    .align 4
used:    .zero 96     /* flags, idx, ring[8] * 8 bytes */
    .align 4
hdr:     .zero 16     /* struct virtio_blk_req: type, reserved, sector */
    .align 4
databuf: .zero 512    /* one sector of data */
    .align 4
statusb: .zero 4      /* the 1-byte completion status (padded) */
