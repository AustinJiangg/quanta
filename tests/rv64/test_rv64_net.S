/* M23: the virtio-net device driven against the usermode network stack
 * (--netdev=user), end to end through the real device and CLI backend.
 *
 * Acting as a bare-metal network driver, this test brings the virtio-net device
 * up (identity, feature handshake, both virtqueues), posts a receive buffer, and
 * transmits an ARP request for the gateway (10.0.2.2). The frame flows
 * device -> the usermode stack -> back into the posted receive buffer, so the
 * test then confirms the ARP reply: it is addressed back to us, comes from the
 * gateway's MAC (52:55:0a:00:02:02), is an ARP reply, and answers for 10.0.2.2.
 * This exercises the transmit->backend->receive path the loopback test cannot —
 * it needs the CLI to attach the stack, so it runs under `make check-net` with
 * `--netdev=user`. Machine-mode + MMIO + virtio, so it is quanta-only.
 *
 * The transmit chain is two descriptors: a zeroed 12-byte virtio-net header the
 * device strips, then the 42-byte ARP frame (a static data blob), so no in-RAM
 * frame building is needed. Completion is synchronous (the stack answers during
 * the transmit notify), so the used rings are read directly.
 */
#include "test_framework.h"

    .equ VIRTIO_MAGIC,        0x000
    .equ VIRTIO_VERSION,      0x004
    .equ VIRTIO_DEVICE_ID,    0x008
    .equ VIRTIO_VENDOR,       0x00c
    .equ VIRTIO_STATUS,       0x070
    .equ VIRTIO_QUEUE_SEL,    0x030
    .equ VIRTIO_QUEUE_NUM_MAX,0x034
    .equ VIRTIO_QUEUE_NUM,    0x038
    .equ VIRTIO_QUEUE_READY,  0x044
    .equ VIRTIO_QUEUE_NOTIFY, 0x050
    .equ VIRTIO_INT_STATUS,   0x060
    .equ VIRTIO_INT_ACK,      0x064
    .equ VIRTIO_DESC_LOW,     0x080
    .equ VIRTIO_DESC_HIGH,    0x084
    .equ VIRTIO_AVAIL_LOW,    0x090
    .equ VIRTIO_AVAIL_HIGH,   0x094
    .equ VIRTIO_USED_LOW,     0x0a0
    .equ VIRTIO_USED_HIGH,    0x0a4

    TEST_START

    li    s0, 0x10002000       /* VIRTIO_NET_BASE */
    la    s1, rxdesc
    la    s2, rxavail
    la    s3, rxused
    la    s4, rxbuf
    la    s5, txdesc
    la    s6, txavail
    la    s7, txused
    la    s8, vnethdr          /* 12-byte zero virtio-net header */
    la    s9, arp_frame        /* the 42-byte ARP request */

    /* ---- device identity ---- */
    lw    t0, VIRTIO_MAGIC(s0)
    CHECK 1, t0, 0x74726976
    lw    t0, VIRTIO_VERSION(s0)
    CHECK 2, t0, 2
    lw    t0, VIRTIO_DEVICE_ID(s0)
    CHECK 3, t0, 1             /* network device */

    /* ---- status handshake (accept no features beyond the defaults) ---- */
    sw    zero, VIRTIO_STATUS(s0)
    li    t0, 1
    sw    t0, VIRTIO_STATUS(s0)         /* ACKNOWLEDGE */
    li    t0, 3
    sw    t0, VIRTIO_STATUS(s0)         /* | DRIVER */
    li    t0, 0xb
    sw    t0, VIRTIO_STATUS(s0)         /* | FEATURES_OK */

    /* ---- queue 0 (receive) ---- */
    sw    zero, VIRTIO_QUEUE_SEL(s0)
    lw    t0, VIRTIO_QUEUE_NUM_MAX(s0)
    CHECK 4, t0, 256
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

    /* ---- queue 1 (transmit) ---- */
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
    sw    t0, VIRTIO_STATUS(s0)         /* DRIVER_OK */

    /* ---- post a receive buffer on queue 0 ---- */
    sd    s4, 0(s1)
    li    t0, 2048
    sw    t0, 8(s1)
    li    t0, 2                        /* WRITE (device fills it) */
    sh    t0, 12(s1)
    sh    zero, 14(s1)
    sh    zero, 4(s2)
    li    t0, 1
    sh    t0, 2(s2)                    /* rxavail.idx = 1 */

    /* ---- transmit the ARP request: [vnethdr | arp_frame] as a 2-desc chain ---- */
    sd    s8, 0(s5)                    /* txdesc[0].addr = vnethdr */
    li    t0, 12
    sw    t0, 8(s5)                    /* len = 12 */
    li    t0, 1                        /* NEXT */
    sh    t0, 12(s5)
    li    t0, 1
    sh    t0, 14(s5)                   /* next = 1 */
    sd    s9, 16(s5)                   /* txdesc[1].addr = arp_frame */
    li    t0, 42
    sw    t0, 24(s5)                   /* len = 42 */
    sh    zero, 28(s5)                 /* flags = 0 (device-readable) */
    sh    zero, 30(s5)
    sh    zero, 4(s6)                  /* txavail.ring[0] = head desc 0 */
    li    t0, 1
    sh    t0, 2(s6)                    /* txavail.idx = 1 */
    li    t0, 1
    sw    t0, VIRTIO_QUEUE_NOTIFY(s0)  /* notify queue 1 (transmit) */

    /* ---- the stack answered into the receive buffer ---- */
    lhu   t0, 2(s3)                   /* receive used.idx advanced */
    CHECK 5, t0, 1
    lhu   t0, 2(s7)                   /* transmit used.idx advanced */
    CHECK 6, t0, 1

    /* the reply frame sits after the 12-byte virtio-net header the device wrote */
    lw    t0, 12(s4)                  /* eth dst[0..3] = our MAC 52:54:00:12 */
    CHECK 7, t0, 0x12005452
    lw    t0, 18(s4)                  /* eth src[0..3] = gateway MAC 52:55:0a:00 */
    CHECK 8, t0, 0x000a5552
    lhu   t0, 22(s4)                  /* eth src[4..5] = 02:02 */
    CHECK 9, t0, 0x0202
    lhu   t0, 24(s4)                  /* ethertype 0x0806 (bytes 08 06) */
    CHECK 10, t0, 0x0608
    lhu   t0, 32(s4)                  /* ARP oper = 2 (bytes 00 02) */
    CHECK 11, t0, 0x0200
    lw    t0, 34(s4)                  /* ARP sha[0..3] = gateway MAC 52:55:0a:00 */
    CHECK 12, t0, 0x000a5552
    lw    t0, 40(s4)                  /* ARP spa = 10.0.2.2 (bytes 0a 00 02 02) */
    CHECK 13, t0, 0x0202000a

    /* the device raised its interrupt for the delivered receive */
    lw    t0, VIRTIO_INT_STATUS(s0)
    andi  t0, t0, 1
    CHECK 14, t0, 1
    li    t0, 1
    sw    t0, VIRTIO_INT_ACK(s0)

    TEST_END

    .data
    .align 4
arp_frame:
    .byte 0xff,0xff,0xff,0xff,0xff,0xff   /* eth dst = broadcast */
    .byte 0x52,0x54,0x00,0x12,0x34,0x56   /* eth src = our MAC */
    .byte 0x08,0x06                       /* ethertype = ARP */
    .byte 0x00,0x01                       /* htype = ethernet */
    .byte 0x08,0x00                       /* ptype = IPv4 */
    .byte 0x06                            /* hlen */
    .byte 0x04                            /* plen */
    .byte 0x00,0x01                       /* oper = request */
    .byte 0x52,0x54,0x00,0x12,0x34,0x56   /* sha = our MAC */
    .byte 10,0,2,15                       /* spa = 10.0.2.15 */
    .byte 0x00,0x00,0x00,0x00,0x00,0x00   /* tha = 0 */
    .byte 10,0,2,2                        /* tpa = 10.0.2.2 (the gateway) */

    .section .bss
    .align 4
vnethdr: .zero 16     /* the 12-byte virtio-net header (device strips it) */
    .align 4
rxdesc:  .zero 128
    .align 4
rxavail: .zero 32
    .align 4
rxused:  .zero 96
    .align 4
rxbuf:   .zero 2048
    .align 4
txdesc:  .zero 128
    .align 4
txavail: .zero 32
    .align 4
txused:  .zero 96
