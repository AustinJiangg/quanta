#ifndef QUANTA_NETSTACK_H
#define QUANTA_NETSTACK_H

#include <stdint.h>

/*
 * A from-scratch usermode network backend for the virtio-net device (M23).
 *
 * The stack presents a virtual gateway on a private network (10.0.2.0/24, the
 * qemu-slirp layout), so a guest can bring an interface up and reach the gateway
 * with no host privileges and no dependency — the from-scratch answer that fits
 * the project's ethos. It is a pure ethernet-frame processor: frames the guest
 * transmits go in through netstack_input, and reply frames come out through the
 * callback given at construction. It holds no reference to the CPU, the memory,
 * or the platform, so it is exercised in isolation by tests/net_test.c; the CLI
 * (--netdev=user) bridges it to the device (device transmit -> netstack_input,
 * netstack out -> the receive path).
 *
 * Implemented so far: Ethernet + ARP (answer for the gateway/DNS IPs), IPv4 with
 * header checksums, ICMP echo (ping the gateway), and a minimal DHCP server
 * (DISCOVER -> OFFER, REQUEST -> ACK) handing the guest 10.0.2.15 with the gateway
 * and DNS. Outbound UDP/TCP NAT to real host sockets and a DNS relay are the next
 * M23 step (they need host sockets, so they are validated against a real guest
 * rather than the deterministic test here).
 */

/* The virtual network the stack presents. Shared with net_test.c so both agree on
 * the addresses; consumers build 4-byte arrays / 6-byte MACs from these. */
#define NS_GATEWAY_IP  { 10, 0, 2, 2 }    /* the gateway/router the stack answers for */
#define NS_DNS_IP      { 10, 0, 2, 3 }    /* the DNS server (advertised; relay is later) */
#define NS_GUEST_IP    { 10, 0, 2, 15 }   /* the address DHCP hands the guest */
#define NS_NETMASK     { 255, 255, 255, 0 }
#define NS_GATEWAY_MAC { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 } /* the gateway's virtual MAC */

typedef struct NetStack NetStack;

/* Create a stack whose reply frames are delivered through `out(ctx, frame, len)`
 * (one whole ethernet frame per call). Returns NULL on allocation failure. */
NetStack *netstack_new(void (*out)(void *ctx, const uint8_t *frame, uint32_t len),
                       void *ctx);

/* Free a stack created by netstack_new (NULL-safe). */
void netstack_free(NetStack *ns);

/* Process one ethernet frame the guest transmitted. Any replies are emitted
 * synchronously through the out callback before this returns. Malformed or
 * unhandled frames are ignored (no reply, never a crash). */
void netstack_input(NetStack *ns, const uint8_t *frame, uint32_t len);

#endif /* QUANTA_NETSTACK_H */
