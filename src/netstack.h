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

/* ----------------------------------------------------------------------------
 * Outbound NAT (M23): reach the real world through host sockets.
 *
 * The virtual-gateway services above are self-contained, but for the guest to
 * talk to hosts *beyond* the gateway (a UDP query, a TCP stream, DNS) the stack
 * needs real sockets. It stays host-independent by delegating all socket I/O to
 * an injected NetIo backend: the POSIX implementation lives in the CLI (main.c),
 * so the protocol logic here remains pure and unit-testable with a mock backend.
 *
 * A NAT "flow" is named by a small integer id the stack allocates (0..NS_NAT_MAX
 * -1); the backend keys its own socket table by that same id, so both sides share
 * one identity space. All backend calls are non-blocking; host-side events come
 * back through the netstack_host_* entry points, driven by the backend's poll.
 * -------------------------------------------------------------------------- */

#define NS_NAT_MAX  64      /* concurrent NAT flows (shared id space with the backend) */
#define NS_PROTO_UDP 0
#define NS_PROTO_TCP 1

typedef struct NetIo {
    void *ctx;
    /* Start outbound flow `id` to remote ip:port (ip network-order, port host
     * order). TCP begins a non-blocking connect (completion -> host_connected).
     * Returns 0 on success, -1 if the socket could not be created. */
    int  (*open)(void *ctx, int id, int proto, const uint8_t ip[4], uint16_t port);
    /* Send payload on flow `id`. Returns bytes accepted (0..len; a TCP socket may
     * take less when its send buffer is full — the stack keeps the rest and
     * retries on host_writable), or -1 on a fatal error. UDP is all-or-nothing. */
    int  (*send)(void *ctx, int id, const uint8_t *data, uint32_t len);
    /* Half-close the send side of a TCP flow (the guest sent FIN). */
    void (*shutdown)(void *ctx, int id);
    /* Close and free flow `id`. */
    void (*close)(void *ctx, int id);
} NetIo;

/* Enable outbound UDP/TCP NAT and DNS relay by attaching a host-I/O backend.
 * `dns` (network order) is the real resolver that packets addressed to the
 * virtual DNS server (10.0.2.3) are relayed to. The NetIo must outlive the
 * stack. Without a backend the stack answers only its virtual gateway. */
void netstack_set_io(NetStack *ns, const NetIo *io, const uint8_t dns[4]);

/* Host-side events the backend delivers from its poll loop, by flow id:
 *  - connected: a TCP connect completed (send the guest its SYN-ACK).
 *  - recv:      bytes arrived (a UDP datagram, or a run of TCP stream bytes).
 *  - closed:    the host end closed the TCP connection (EOF -> FIN to guest).
 *  - reset:     the host flow failed or was reset (connect refused, RST, error).
 *  - writable:  the host socket can accept more — retry buffered guest->host data. */
void netstack_host_connected(NetStack *ns, int id);
void netstack_host_recv(NetStack *ns, int id, const uint8_t *data, uint32_t len);
void netstack_host_closed(NetStack *ns, int id);
void netstack_host_reset(NetStack *ns, int id);
void netstack_host_writable(NetStack *ns, int id);

/* Poll-loop queries the backend uses to build its fd sets:
 *  - rx_room:     bytes the backend may read for flow id now (0 = apply
 *                 backpressure: the guest's receive window / buffer is full).
 *  - wants_write: flow id has buffered guest->host data awaiting host writability. */
uint32_t netstack_rx_room(NetStack *ns, int id);
int      netstack_wants_write(NetStack *ns, int id);

#endif /* QUANTA_NETSTACK_H */
