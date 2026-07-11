/* M23: the virtio-mmio network device (modern / version 2), driven end to end
 * through its internal loopback backend.
 *
 * Acting as a bare-metal (machine-mode) network driver, this test:
 *
 *   1. probes the device identity (magic / version / device-id 1 / vendor),
 *   2. runs the status + feature handshake (reset, ACKNOWLEDGE, DRIVER, accept
 *      VIRTIO_NET_F_MAC + VIRTIO_F_VERSION_1, FEATURES_OK) and confirms it stuck,
 *   3. reads the 6-byte MAC from config space (VIRTIO_NET_F_MAC),
 *   4. sets up both split virtqueues (0 = receive, 1 = transmit) in RAM,
 *   5. posts a receive buffer on queue 0, then transmits a frame on queue 1,
 *   6. confirms the loopback delivered the transmitted frame back into the posted
 *      receive buffer: both used rings advanced, the received length is the
 *      12-byte virtio-net header plus the frame, the payload matches, and the
 *      virtio-net header's num_buffers is 1,
 *   7. confirms the device raised its interrupt (mmio status + PLIC source 2),
 *      then acknowledges it.
 *
 * With no host backend attached the device loops a transmitted frame straight
 * back to the receive queue, so completion is synchronous on QUEUE_NOTIFY and the
 * test reads the used rings directly rather than spinning. It stays in M-mode
 * throughout (Bare translation, so the addresses it hands the device are the
 * physical ones); machine-mode + MMIO + virtio means user-mode qemu can't run it,
 * so it is quanta-only (make check-virtnet), not part of the qemu-riscv64
 * differential.
 */
#include "test_framework.h"

    /* virtio-mmio register offsets (a driver's subset). */
    .equ VIRTIO_MAGIC,          0x000
    .equ VIRTIO_VERSION,        0x004
    .equ VIRTIO_DEVICE_ID,      0x008
    .equ VIRTIO_VENDOR,         0x00c
    .equ VIRTIO_DEVICE_FEAT,    0x010
    .equ VIRTIO_DEVICE_FEAT_SEL,0x014
    .equ VIRTIO_DRIVER_FEAT,    0x020
    .equ VIRTIO_DRIVER_FEAT_SEL,0x024
    .equ VIRTIO_QUEUE_SEL,      0x030
    .equ VIRTIO_QUEUE_NUM_MAX,  0x034
    .equ VIRTIO_QUEUE_NUM,      0x038
    .equ VIRTIO_QUEUE_READY,    0x044
    .equ VIRTIO_QUEUE_NOTIFY,   0x050
    .equ VIRTIO_INT_STATUS,     0x060
    .equ VIRTIO_INT_ACK,        0x064
    .equ VIRTIO_STATUS,         0x070
    .equ VIRTIO_DESC_LOW,       0x080
    .equ VIRTIO_DESC_HIGH,      0x084
    .equ VIRTIO_AVAIL_LOW,      0x090
    .equ VIRTIO_AVAIL_HIGH,     0x094
    .equ VIRTIO_USED_LOW,       0x0a0
    .equ VIRTIO_USED_HIGH,      0x0a4
    .equ VIRTIO_CONFIG,         0x100

    .equ PLIC_PENDING,          0x0c001000   /* pending bitmap, sources 0..31 */

    TEST_START

    li    s0, 0x10002000       /* VIRTIO_NET_BASE (second virtio slot) */
    la    s1, rxdesc
    la    s2, rxavail
    la    s3, rxused
    la    s4, rxbuf
    la    s5, txdesc
    la    s6, txavail
    la    s7, txused
    la    s8, txbuf

    /* ---- 1. device identity ---- */
    lw    t0, VIRTIO_MAGIC(s0)
    CHECK 1, t0, 0x74726976     /* "virt" */
    lw    t0, VIRTIO_VERSION(s0)
    CHECK 2, t0, 2              /* modern (v2) */
    lw    t0, VIRTIO_DEVICE_ID(s0)
    CHECK 3, t0, 1             /* network device */
    lw    t0, VIRTIO_VENDOR(s0)
    CHECK 4, t0, 0x554d4551    /* "QEMU" */

    /* ---- 2. status handshake + feature negotiation ---- */
    sw    zero, VIRTIO_STATUS(s0)       /* reset */
    li    t0, 1
    sw    t0, VIRTIO_STATUS(s0)         /* ACKNOWLEDGE */
    li    t0, 3
    sw    t0, VIRTIO_STATUS(s0)         /* | DRIVER */
    /* device features: low half advertises F_MAC (bit 5); high half VERSION_1 */
    sw    zero, VIRTIO_DEVICE_FEAT_SEL(s0)
    lw    t0, VIRTIO_DEVICE_FEAT(s0)
    CHECK 5, t0, 0x20                   /* 1 << VIRTIO_NET_F_MAC */
    li    t0, 1
    sw    t0, VIRTIO_DEVICE_FEAT_SEL(s0)
    lw    t0, VIRTIO_DEVICE_FEAT(s0)
    CHECK 6, t0, 1                      /* VIRTIO_F_VERSION_1 (bit 32) */
    /* accept F_MAC (low) and VERSION_1 (high) */
    sw    zero, VIRTIO_DRIVER_FEAT_SEL(s0)
    li    t0, 0x20
    sw    t0, VIRTIO_DRIVER_FEAT(s0)
    li    t0, 1
    sw    t0, VIRTIO_DRIVER_FEAT_SEL(s0)
    li    t0, 1
    sw    t0, VIRTIO_DRIVER_FEAT(s0)
    li    t0, 0xb
    sw    t0, VIRTIO_STATUS(s0)         /* | FEATURES_OK */
    lw    t0, VIRTIO_STATUS(s0)
    andi  t0, t0, 8
    CHECK 7, t0, 8                      /* FEATURES_OK stuck */

    /* ---- 3. read the MAC from config space (52:54:00:12:34:56) ---- */
    lbu   t0, VIRTIO_CONFIG+0(s0)
    CHECK 8, t0, 0x52
    lbu   t0, VIRTIO_CONFIG+1(s0)
    CHECK 9, t0, 0x54
    lbu   t0, VIRTIO_CONFIG+2(s0)
    CHECK 10, t0, 0x00
    lbu   t0, VIRTIO_CONFIG+3(s0)
    CHECK 11, t0, 0x12
    lbu   t0, VIRTIO_CONFIG+4(s0)
    CHECK 12, t0, 0x34
    lbu   t0, VIRTIO_CONFIG+5(s0)
    CHECK 13, t0, 0x56

    /* ---- 4. set up queue 0 (receive) ---- */
    sw    zero, VIRTIO_QUEUE_SEL(s0)
    lw    t0, VIRTIO_QUEUE_NUM_MAX(s0)
    CHECK 14, t0, 256                   /* advertised ring capacity */
    li    t0, 8
    sw    t0, VIRTIO_QUEUE_NUM(s0)
    sw    s1, VIRTIO_DESC_LOW(s0)
    srli  t0, s1, 32
    sw    t0, VIRTIO_DESC_HIGH(s0)
    sw    s2, VIRTIO_AVAIL_LOW(s0)
    srli  t0, s2, 32
    sw    t0, VIRTIO_AVAIL_HIGH(s0)
    sw    s3, VIRTIO_USED_LOW(s0)
    srli  t0, s3, 32
    sw    t0, VIRTIO_USED_HIGH(s0)
    li    t0, 1
    sw    t0, VIRTIO_QUEUE_READY(s0)

    /* ---- set up queue 1 (transmit) ---- */
    li    t0, 1
    sw    t0, VIRTIO_QUEUE_SEL(s0)
    li    t0, 8
    sw    t0, VIRTIO_QUEUE_NUM(s0)
    sw    s5, VIRTIO_DESC_LOW(s0)
    srli  t0, s5, 32
    sw    t0, VIRTIO_DESC_HIGH(s0)
    sw    s6, VIRTIO_AVAIL_LOW(s0)
    srli  t0, s6, 32
    sw    t0, VIRTIO_AVAIL_HIGH(s0)
    sw    s7, VIRTIO_USED_LOW(s0)
    srli  t0, s7, 32
    sw    t0, VIRTIO_USED_HIGH(s0)
    li    t0, 1
    sw    t0, VIRTIO_QUEUE_READY(s0)

    li    t0, 0xf
    sw    t0, VIRTIO_STATUS(s0)         /* | DRIVER_OK */

    /* ---- 5. post a receive buffer on queue 0 ----
     * rxdesc[0] = { rxbuf, 2048, WRITE, next 0 }; avail.ring[0] = 0, avail.idx = 1. */
    sd    s4, 0(s1)
    li    t0, 2048
    sw    t0, 8(s1)
    li    t0, 2                        /* VRING_DESC_F_WRITE (device fills it) */
    sh    t0, 12(s1)
    sh    zero, 14(s1)
    sh    zero, 4(s2)                  /* avail.ring[0] = head desc 0 */
    li    t0, 1
    sh    t0, 2(s2)                    /* avail.idx = 1 */
    sw    zero, VIRTIO_QUEUE_NOTIFY(s0) /* notify queue 0 (no frame yet: a no-op) */

    /* ---- build and transmit a frame on queue 1 ----
     * txbuf = [12-byte virtio-net header (zeroed) | ethernet frame]. */
    li    t0, 0x12345678
    sw    t0, 12(s8)                   /* frame word 0 */
    li    t0, 0x5a5a5a5a
    sw    t0, 16(s8)                   /* frame word 1 */
    sd    s8, 0(s5)                    /* txdesc[0].addr = txbuf */
    li    t0, 76                       /* 12 header + 64 frame */
    sw    t0, 8(s5)
    sh    zero, 12(s5)                 /* flags = 0 (device-readable) */
    sh    zero, 14(s5)
    sh    zero, 4(s6)                  /* avail.ring[0] = head desc 0 */
    li    t0, 1
    sh    t0, 2(s6)                    /* avail.idx = 1 */
    li    t0, 1
    sw    t0, VIRTIO_QUEUE_NOTIFY(s0)  /* notify queue 1 (transmit) */

    /* ---- 6. the loopback delivered the frame back to receive ---- */
    lhu   t0, 2(s7)                   /* transmit used.idx advanced */
    CHECK 15, t0, 1
    lhu   t0, 2(s3)                   /* receive used.idx advanced */
    CHECK 16, t0, 1
    lw    t0, 8(s3)                   /* receive used.ring[0].len = 12 + 64 */
    CHECK 17, t0, 76
    lhu   t0, 10(s4)                  /* virtio-net header num_buffers = 1 */
    CHECK 18, t0, 1
    lw    t0, 12(s4)                  /* frame word 0 (past the 12-byte header) */
    CHECK 19, t0, 0x12345678
    lw    t0, 16(s4)                  /* frame word 1 */
    CHECK 20, t0, 0x5a5a5a5a

    /* ---- 7. the device raised its interrupt (mmio status + PLIC source 2) ---- */
    lw    t0, VIRTIO_INT_STATUS(s0)
    andi  t0, t0, 1
    CHECK 21, t0, 1
    li    t1, PLIC_PENDING
    lw    t0, 0(t1)
    andi  t0, t0, 4                   /* 1 << VIRTIO_NET_IRQ */
    CHECK 22, t0, 4
    li    t0, 1
    sw    t0, VIRTIO_INT_ACK(s0)       /* acknowledge -> device deasserts */
    lw    t0, VIRTIO_INT_STATUS(s0)
    CHECK 23, t0, 0

    TEST_END

    .section .bss
    .align 4
rxdesc:  .zero 128    /* 8 descriptors * 16 bytes */
    .align 4
rxavail: .zero 32     /* flags, idx, ring[8], used_event */
    .align 4
rxused:  .zero 96     /* flags, idx, ring[8] * 8 bytes */
    .align 4
rxbuf:   .zero 2048   /* one receive buffer (virtio-net header + frame) */
    .align 4
txdesc:  .zero 128
    .align 4
txavail: .zero 32
    .align 4
txused:  .zero 96
    .align 4
txbuf:   .zero 2048   /* virtio-net header + frame to transmit */
