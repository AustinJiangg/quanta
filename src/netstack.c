#include "netstack.h"

#include <stdlib.h>
#include <string.h>

/*
 * The usermode network stack — see netstack.h for the overview.
 *
 * Two layers share this file:
 *   1. The virtual gateway (ARP, ICMP echo, DHCP) answers locally and needs no
 *      host networking — a pure ethernet-frame processor.
 *   2. Outbound NAT (UDP/TCP + DNS relay) bridges the guest to real host sockets
 *      through the injected NetIo backend. The stack terminates the guest's TCP
 *      itself (a small state machine) and streams bytes to/from a host socket.
 *
 * Everything is bounds-checked against the frame length, since the frames are
 * guest-controlled. A key simplification: the virtio link between guest and
 * stack is synchronous and lossless (a transmitted frame is delivered in full,
 * in order, immediately), so the TCP bridge needs no retransmission timers — it
 * only keeps correct sequence/ack bookkeeping and uses the advertised windows
 * for flow control.
 */

#define NS_MTU 1600

/* Ethernet header. */
#define ETH_DST  0
#define ETH_SRC  6
#define ETH_TYPE 12
#define ETH_HDR  14
#define ETYPE_IP  0x0800u
#define ETYPE_ARP 0x0806u

#define PROTO_ICMP 1u
#define PROTO_TCP  6u
#define PROTO_UDP  17u

/* TCP flags. */
#define TCP_FIN 0x01u
#define TCP_SYN 0x02u
#define TCP_RST 0x04u
#define TCP_ACK 0x10u

/* Per-flow TCP buffering / advertised window, and the segment payload cap. */
#define NS_TCP_BUF 32768u
#define NS_TCP_MSS 1460u

/* The virtual network's fixed addresses (from netstack.h). */
static const uint8_t gw_mac[6]     = NS_GATEWAY_MAC;
static const uint8_t gw_ip[4]      = NS_GATEWAY_IP;
static const uint8_t dns_ip[4]     = NS_DNS_IP;
static const uint8_t guest_ip[4]   = NS_GUEST_IP;
static const uint8_t netmask[4]    = NS_NETMASK;
static const uint8_t dhcp_magic[4] = { 0x63, 0x82, 0x53, 0x63 };

/* A NAT flow: the guest's connection/datagram stream to one remote endpoint. */
enum { TS_FREE = 0, TS_CONNECTING, TS_SYNRCVD, TS_ESTAB };
typedef struct {
    int      proto;          /* NS_PROTO_UDP / NS_PROTO_TCP; TS_FREE marks unused */
    uint8_t  face_ip[4];     /* remote IP the guest addressed (reply src; DNS=10.0.2.3) */
    uint8_t  real_ip[4];     /* where the backend actually connects (DNS -> upstream) */
    uint16_t guest_port;     /* guest's source port */
    uint16_t face_port;      /* remote port */
    /* --- TCP only --- */
    int      state;
    int      guest_fin;      /* the guest sent FIN (its send side is done) */
    int      host_eof;       /* the host closed (its send side is done) */
    int      fin_sent;       /* we sent our FIN to the guest */
    int      shutdown_pending; /* guest FIN seen; shutdown host send once g2h drains */
    uint32_t snd_nxt;        /* next sequence number we send to the guest */
    uint32_t snd_una;        /* oldest sequence the guest has not acknowledged */
    uint32_t rcv_nxt;        /* next sequence we expect from the guest */
    uint16_t guest_wnd;      /* the guest's advertised receive window */
    uint8_t *g2h;            /* guest->host bytes awaiting host writability */
    uint32_t g2h_len;
    uint8_t *h2g;            /* host->guest bytes awaiting the guest's window */
    uint32_t h2g_len;
} NatFlow;

struct NetStack {
    void   (*out)(void *ctx, const uint8_t *frame, uint32_t len);
    void    *ctx;
    uint8_t  guest_mac[6];   /* learned from the guest's frames */
    int      have_guest_mac;
    uint16_t ip_id;          /* IP identification counter for the frames we emit */
    const NetIo *io;         /* NULL until netstack_set_io: gateway-only, no NAT */
    uint8_t  upstream_dns[4];/* real resolver DNS packets are relayed to */
    uint32_t tcp_isn;        /* rolling initial sequence number for our TCP side */
    NatFlow  flows[NS_NAT_MAX];
};

/* Big-endian (network byte order) field access. */
static uint16_t be16(const uint8_t *p) { return (uint16_t)((uint32_t)p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* The internet checksum (RFC 1071): one's-complement sum of 16-bit words. */
static uint16_t inet_csum(const uint8_t *data, uint32_t len) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i + 1 < len; i += 2)
        sum += (uint32_t)data[i] << 8 | data[i + 1];
    if (len & 1) sum += (uint32_t)data[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

/* The TCP/UDP checksum: the RFC 1071 sum over the IPv4 pseudo-header plus L4. */
static uint16_t l4_csum(const uint8_t *src, const uint8_t *dst, uint8_t proto,
                        const uint8_t *l4, uint32_t l4len) {
    uint32_t sum = 0;
    sum += (uint32_t)src[0] << 8 | src[1]; sum += (uint32_t)src[2] << 8 | src[3];
    sum += (uint32_t)dst[0] << 8 | dst[1]; sum += (uint32_t)dst[2] << 8 | dst[3];
    sum += proto;
    sum += l4len;
    for (uint32_t i = 0; i + 1 < l4len; i += 2)
        sum += (uint32_t)l4[i] << 8 | l4[i + 1];
    if (l4len & 1) sum += (uint32_t)l4[l4len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Is `ip` one of the addresses the gateway answers for? */
static int owns_ip(const uint8_t *ip) {
    return memcmp(ip, gw_ip, 4) == 0 || memcmp(ip, dns_ip, 4) == 0;
}

/* ---- ARP: reply to a request for one of our IPs with the gateway MAC. ---- */
static void handle_arp(NetStack *ns, const uint8_t *frame, uint32_t len) {
    if (len < ETH_HDR + 28) return;
    const uint8_t *a = frame + ETH_HDR;
    if (be16(a) != 1 || be16(a + 2) != ETYPE_IP) return; /* ethernet / IPv4 */
    if (a[4] != 6 || a[5] != 4) return;                  /* hlen / plen */
    if (be16(a + 6) != 1) return;                        /* oper = request */
    const uint8_t *sha = a + 8, *spa = a + 14, *tpa = a + 24;
    if (!owns_ip(tpa)) return;

    uint8_t out[ETH_HDR + 28];
    memcpy(out + ETH_DST, sha, 6);
    memcpy(out + ETH_SRC, gw_mac, 6);
    put_be16(out + ETH_TYPE, ETYPE_ARP);
    uint8_t *r = out + ETH_HDR;
    put_be16(r, 1); put_be16(r + 2, ETYPE_IP); r[4] = 6; r[5] = 4;
    put_be16(r + 6, 2);                 /* reply */
    memcpy(r + 8, gw_mac, 6);           /* sha = gateway MAC */
    memcpy(r + 14, tpa, 4);             /* spa = the IP that was asked for */
    memcpy(r + 18, sha, 6);             /* tha = requester MAC */
    memcpy(r + 24, spa, 4);             /* tpa = requester IP */
    ns->out(ns->ctx, out, sizeof out);
}

/* ---- ICMP: reply to an echo request aimed at the gateway. ---- */
static void handle_icmp(NetStack *ns, const uint8_t *frame, uint32_t ipoff,
                        uint32_t ihl, uint32_t iptotal) {
    const uint8_t *ip = frame + ipoff;
    if (iptotal < ihl + 8) return;
    uint32_t icmp_len = iptotal - ihl;
    const uint8_t *icmp = frame + ipoff + ihl;
    if (icmp[0] != 8 || icmp[1] != 0) return;    /* echo request */
    if (memcmp(ip + 16, gw_ip, 4) != 0) return;  /* destined to the gateway */
    if (icmp_len > NS_MTU - ETH_HDR - 20) return;

    uint8_t out[NS_MTU];
    uint32_t iptot = 20 + icmp_len;
    memcpy(out + ETH_DST, frame + ETH_SRC, 6);   /* back to the sender */
    memcpy(out + ETH_SRC, gw_mac, 6);
    put_be16(out + ETH_TYPE, ETYPE_IP);

    uint8_t *o = out + ETH_HDR;                   /* IPv4 header */
    o[0] = 0x45; o[1] = 0; put_be16(o + 2, (uint16_t)iptot);
    put_be16(o + 4, ns->ip_id++); put_be16(o + 6, 0);
    o[8] = 64; o[9] = (uint8_t)PROTO_ICMP; put_be16(o + 10, 0);
    memcpy(o + 12, gw_ip, 4);                     /* src = gateway */
    memcpy(o + 16, ip + 12, 4);                   /* dst = original source */
    put_be16(o + 10, inet_csum(o, 20));

    uint8_t *ic = o + 20;                          /* ICMP: request with type 0 */
    memcpy(ic, icmp, icmp_len);
    ic[0] = 0;                                     /* echo reply */
    put_be16(ic + 2, 0);
    put_be16(ic + 2, inet_csum(ic, icmp_len));
    ns->out(ns->ctx, out, ETH_HDR + iptot);
}

/* ---- DHCP: a minimal server, DISCOVER -> OFFER and REQUEST -> ACK. ---- */

/* Find the DHCP message-type option (53) in the options block. 0 = not found. */
static uint8_t dhcp_msgtype(const uint8_t *opt, uint32_t len) {
    uint32_t i = 0;
    while (i < len) {
        uint8_t code = opt[i++];
        if (code == 0) continue;         /* pad */
        if (code == 255) break;          /* end */
        if (i >= len) break;
        uint8_t l = opt[i++];
        if (i + l > len) break;
        if (code == 53 && l >= 1) return opt[i];
        i += l;
    }
    return 0;
}

static void handle_dhcp(NetStack *ns, const uint8_t *frame, uint32_t udpoff,
                        uint32_t udp_total) {
    if (udp_total < 8 + 240) return;              /* need the fixed BOOTP fields */
    const uint8_t *dhcp = frame + udpoff + 8;
    uint32_t dhcp_len = udp_total - 8;
    if (memcmp(dhcp + 236, dhcp_magic, 4) != 0) return;

    uint8_t reply_type;
    switch (dhcp_msgtype(dhcp + 240, dhcp_len - 240)) {
        case 1:  reply_type = 2; break;           /* DISCOVER -> OFFER */
        case 3:  reply_type = 5; break;           /* REQUEST  -> ACK   */
        default: return;
    }

    uint8_t out[NS_MTU];
    memset(out, 0, sizeof out);
    memset(out + ETH_DST, 0xff, 6);               /* broadcast (client has no IP yet) */
    memcpy(out + ETH_SRC, gw_mac, 6);
    put_be16(out + ETH_TYPE, ETYPE_IP);

    uint32_t dhcp_body = 300;                     /* pad the BOOTP body to 300 bytes */
    uint32_t udp_len = 8 + dhcp_body;
    uint32_t ip_len  = 20 + udp_len;

    uint8_t *ip = out + ETH_HDR;                  /* IPv4 header */
    ip[0] = 0x45; put_be16(ip + 2, (uint16_t)ip_len);
    put_be16(ip + 4, ns->ip_id++);
    ip[8] = 64; ip[9] = (uint8_t)PROTO_UDP;
    memcpy(ip + 12, gw_ip, 4);                    /* src = gateway (the server) */
    memset(ip + 16, 0xff, 4);                     /* dst = 255.255.255.255 */
    put_be16(ip + 10, inet_csum(ip, 20));

    uint8_t *udp = ip + 20;                        /* UDP header */
    put_be16(udp, 67); put_be16(udp + 2, 68);
    put_be16(udp + 4, (uint16_t)udp_len);
    put_be16(udp + 6, 0);                          /* checksum optional on IPv4 */

    uint8_t *d = udp + 8;                          /* the DHCP message (zeroed) */
    d[0] = 2; d[1] = 1; d[2] = 6;                  /* BOOTREPLY, ethernet, hlen 6 */
    memcpy(d + 4,  dhcp + 4,  4);                  /* xid (echo the request's) */
    memcpy(d + 10, dhcp + 10, 2);                  /* flags (echo) */
    memcpy(d + 16, guest_ip,  4);                  /* yiaddr = the offered address */
    memcpy(d + 20, gw_ip,     4);                  /* siaddr = the server */
    memcpy(d + 28, dhcp + 28, 6);                  /* chaddr = client MAC */
    memcpy(d + 236, dhcp_magic, 4);

    uint8_t *o = d + 240;                          /* options */
    *o++ = 53; *o++ = 1; *o++ = reply_type;        /* message type */
    *o++ = 54; *o++ = 4; memcpy(o, gw_ip, 4);   o += 4;  /* server identifier */
    *o++ = 51; *o++ = 4; put_be32(o, 86400);    o += 4;  /* lease time (1 day) */
    *o++ = 1;  *o++ = 4; memcpy(o, netmask, 4); o += 4;  /* subnet mask */
    *o++ = 3;  *o++ = 4; memcpy(o, gw_ip, 4);   o += 4;  /* router */
    *o++ = 6;  *o++ = 4; memcpy(o, dns_ip, 4);  o += 4;  /* DNS server */
    *o++ = 255;                                    /* end (rest stays pad-zero) */

    ns->out(ns->ctx, out, ETH_HDR + ip_len);
}

/* ---- NAT flow table ---- */

static int flow_id(const NetStack *ns, const NatFlow *f) { return (int)(f - ns->flows); }

/* Find the flow matching (proto, guest src port, remote ip:port), or NULL. */
static NatFlow *flow_find(NetStack *ns, int proto, uint16_t gport,
                          const uint8_t *rip, uint16_t rport) {
    for (int i = 0; i < NS_NAT_MAX; i++) {
        NatFlow *f = &ns->flows[i];
        if (f->state == TS_FREE || f->proto != proto) continue;
        if (f->guest_port == gport && f->face_port == rport &&
            memcmp(f->face_ip, rip, 4) == 0)
            return f;
    }
    return NULL;
}

/* Allocate a flow to remote ip:port for the guest's source port. DNS-relay
 * rewrite: a flow to the virtual DNS server connects to the real upstream while
 * still presenting the DNS server's address to the guest. NULL if the table is
 * full or no backend is attached. */
static NatFlow *flow_alloc(NetStack *ns, int proto, uint16_t gport,
                           const uint8_t *rip, uint16_t rport) {
    if (!ns->io) return NULL;
    for (int i = 0; i < NS_NAT_MAX; i++) {
        NatFlow *f = &ns->flows[i];
        if (f->state != TS_FREE) continue;
        memset(f, 0, sizeof *f);
        f->proto = proto;
        f->guest_port = gport;
        f->face_port = rport;
        memcpy(f->face_ip, rip, 4);
        if (memcmp(rip, dns_ip, 4) == 0) memcpy(f->real_ip, ns->upstream_dns, 4);
        else                             memcpy(f->real_ip, rip, 4);
        f->state = (proto == NS_PROTO_TCP) ? TS_CONNECTING : TS_ESTAB;
        return f;
    }
    return NULL;
}

static void flow_free(NetStack *ns, NatFlow *f) {
    if (f->state == TS_FREE) return;
    if (ns->io) ns->io->close(ns->io->ctx, flow_id(ns, f));
    free(f->g2h);
    free(f->h2g);
    memset(f, 0, sizeof *f);              /* state = TS_FREE */
}

/* ---- UDP NAT ---- */

/* Deliver a datagram received from the host back to the guest as UDP. */
static void udp_to_guest(NetStack *ns, NatFlow *f, const uint8_t *data, uint32_t len) {
    if (len > NS_MTU - ETH_HDR - 20 - 8) len = NS_MTU - ETH_HDR - 20 - 8;
    uint8_t out[NS_MTU];
    uint32_t udp_len = 8 + len;
    uint32_t ip_len  = 20 + udp_len;
    memcpy(out + ETH_DST, ns->guest_mac, 6);
    memcpy(out + ETH_SRC, gw_mac, 6);
    put_be16(out + ETH_TYPE, ETYPE_IP);

    uint8_t *ip = out + ETH_HDR;
    ip[0] = 0x45; ip[1] = 0; put_be16(ip + 2, (uint16_t)ip_len);
    put_be16(ip + 4, ns->ip_id++); put_be16(ip + 6, 0);
    ip[8] = 64; ip[9] = (uint8_t)PROTO_UDP; put_be16(ip + 10, 0);
    memcpy(ip + 12, f->face_ip, 4);              /* src = the remote the guest addressed */
    memcpy(ip + 16, guest_ip, 4);                /* dst = guest */
    put_be16(ip + 10, inet_csum(ip, 20));

    uint8_t *udp = ip + 20;
    put_be16(udp, f->face_port); put_be16(udp + 2, f->guest_port);
    put_be16(udp + 4, (uint16_t)udp_len);
    put_be16(udp + 6, 0);                        /* checksum optional on IPv4 */
    memcpy(udp + 8, data, len);
    ns->out(ns->ctx, out, ETH_HDR + ip_len);
}

/* A guest UDP datagram to a non-local address: open/reuse a flow and send it. */
static void udp_nat(NetStack *ns, const uint8_t *frame, uint32_t ipoff, uint32_t ihl,
                    uint32_t iptotal) {
    if (!ns->io || iptotal < ihl + 8) return;
    const uint8_t *ip = frame + ipoff;
    const uint8_t *udp = frame + ipoff + ihl;
    uint16_t sport = be16(udp), dport = be16(udp + 2);
    uint32_t ulen = be16(udp + 4);
    if (ulen < 8 || ulen > iptotal - ihl) ulen = iptotal - ihl; /* trust the frame */
    const uint8_t *payload = udp + 8;
    uint32_t plen = ulen - 8;

    NatFlow *f = flow_find(ns, NS_PROTO_UDP, sport, ip + 16, dport);
    if (!f) {
        f = flow_alloc(ns, NS_PROTO_UDP, sport, ip + 16, dport);
        if (!f) return;
        if (ns->io->open(ns->io->ctx, flow_id(ns, f), NS_PROTO_UDP,
                         f->real_ip, dport) != 0) { flow_free(ns, f); return; }
    }
    ns->io->send(ns->io->ctx, flow_id(ns, f), payload, plen);
}

/* ---- TCP NAT (the stack terminates the guest's connection) ---- */

/* Emit one TCP segment to the guest with the flow's current seq/ack/window.
 * `flags` should include TCP_ACK for anything past the initial handshake. */
static void tcp_out(NetStack *ns, NatFlow *f, uint8_t flags,
                    const uint8_t *payload, uint32_t plen) {
    uint8_t out[ETH_HDR + 20 + 20 + NS_TCP_MSS];
    memcpy(out + ETH_DST, ns->guest_mac, 6);
    memcpy(out + ETH_SRC, gw_mac, 6);
    put_be16(out + ETH_TYPE, ETYPE_IP);

    uint32_t iptot = 20 + 20 + plen;
    uint8_t *ip = out + ETH_HDR;
    ip[0] = 0x45; ip[1] = 0; put_be16(ip + 2, (uint16_t)iptot);
    put_be16(ip + 4, ns->ip_id++); put_be16(ip + 6, 0x4000); /* don't fragment */
    ip[8] = 64; ip[9] = (uint8_t)PROTO_TCP; put_be16(ip + 10, 0);
    memcpy(ip + 12, f->face_ip, 4);              /* src = remote endpoint */
    memcpy(ip + 16, guest_ip, 4);                /* dst = guest */
    put_be16(ip + 10, inet_csum(ip, 20));

    uint8_t *t = ip + 20;
    put_be16(t + 0, f->face_port);               /* src port = remote */
    put_be16(t + 2, f->guest_port);              /* dst port = guest */
    put_be32(t + 4, f->snd_nxt);
    put_be32(t + 8, f->rcv_nxt);
    t[12] = 5 << 4;                              /* data offset = 5 words, no options */
    t[13] = flags;
    put_be16(t + 14, (uint16_t)(NS_TCP_BUF - f->g2h_len)); /* our receive window */
    put_be16(t + 16, 0);                         /* checksum, filled below */
    put_be16(t + 18, 0);                         /* urgent pointer */
    if (plen) memcpy(t + 20, payload, plen);
    put_be16(t + 16, l4_csum(f->face_ip, guest_ip, (uint8_t)PROTO_TCP, t, 20 + plen));
    ns->out(ns->ctx, out, ETH_HDR + iptot);
}

/* The SYN-ACK carries a 4-byte MSS option so the guest sends full-size segments. */
static void tcp_synack(NetStack *ns, NatFlow *f) {
    uint8_t out[ETH_HDR + 20 + 24];
    memcpy(out + ETH_DST, ns->guest_mac, 6);
    memcpy(out + ETH_SRC, gw_mac, 6);
    put_be16(out + ETH_TYPE, ETYPE_IP);

    uint32_t iptot = 20 + 24;                    /* IP + TCP(20) + MSS option(4) */
    uint8_t *ip = out + ETH_HDR;
    ip[0] = 0x45; ip[1] = 0; put_be16(ip + 2, (uint16_t)iptot);
    put_be16(ip + 4, ns->ip_id++); put_be16(ip + 6, 0x4000);
    ip[8] = 64; ip[9] = (uint8_t)PROTO_TCP; put_be16(ip + 10, 0);
    memcpy(ip + 12, f->face_ip, 4);
    memcpy(ip + 16, guest_ip, 4);
    put_be16(ip + 10, inet_csum(ip, 20));

    uint8_t *t = ip + 20;
    put_be16(t + 0, f->face_port); put_be16(t + 2, f->guest_port);
    put_be32(t + 4, f->snd_nxt);                 /* our ISN */
    put_be32(t + 8, f->rcv_nxt);                 /* ack = guest ISN + 1 */
    t[12] = 6 << 4;                              /* data offset = 6 words (24 bytes) */
    t[13] = (uint8_t)(TCP_SYN | TCP_ACK);
    put_be16(t + 14, (uint16_t)NS_TCP_BUF);
    put_be16(t + 16, 0); put_be16(t + 18, 0);
    t[20] = 2; t[21] = 4; put_be16(t + 22, (uint16_t)NS_TCP_MSS); /* MSS option */
    put_be16(t + 16, l4_csum(f->face_ip, guest_ip, (uint8_t)PROTO_TCP, t, 24));
    ns->out(ns->ctx, out, ETH_HDR + iptot);
}

/* Push as much buffered host->guest data as the guest's window allows. */
static void tcp_flush_h2g(NetStack *ns, NatFlow *f) {
    if (f->state != TS_ESTAB) return;
    while (f->h2g && f->h2g_len > 0) {
        uint32_t inflight = f->snd_nxt - f->snd_una;
        if (inflight >= f->guest_wnd) break;                 /* window full */
        uint32_t room = f->guest_wnd - inflight;
        uint32_t chunk = f->h2g_len;
        if (chunk > room)       chunk = room;
        if (chunk > NS_TCP_MSS) chunk = NS_TCP_MSS;
        tcp_out(ns, f, TCP_ACK, f->h2g, chunk);
        f->snd_nxt += chunk;
        memmove(f->h2g, f->h2g + chunk, f->h2g_len - chunk); /* drop sent bytes */
        f->h2g_len -= chunk;
    }
    /* When the host is done and everything is sent, send our FIN. */
    if (f->host_eof && f->h2g_len == 0 && !f->fin_sent) {
        tcp_out(ns, f, (uint8_t)(TCP_FIN | TCP_ACK), NULL, 0);
        f->snd_nxt += 1;
        f->fin_sent = 1;
    }
}

/* Drain buffered guest->host bytes to the host socket, and honour a pending
 * half-close once the buffer empties. Returns 1 if the flow is still alive, 0 if
 * a fatal send error tore it down (the caller must then stop touching it). The
 * window-update ACK is left to the caller, which knows whether it will ACK anyway. */
static int tcp_drain_g2h(NetStack *ns, NatFlow *f) {
    if (f->g2h && f->g2h_len > 0) {
        int n = ns->io->send(ns->io->ctx, flow_id(ns, f), f->g2h, f->g2h_len);
        if (n < 0) { tcp_out(ns, f, TCP_RST, NULL, 0); flow_free(ns, f); return 0; }
        if (n > 0) {
            memmove(f->g2h, f->g2h + n, f->g2h_len - (uint32_t)n);
            f->g2h_len -= (uint32_t)n;
        }
    }
    if (f->shutdown_pending && f->g2h_len == 0) {
        ns->io->shutdown(ns->io->ctx, flow_id(ns, f));
        f->shutdown_pending = 0;
    }
    return 1;
}

/* Tear down once both directions are closed and nothing is left to deliver. */
static void tcp_maybe_close(NetStack *ns, NatFlow *f) {
    if (f->guest_fin && f->fin_sent && f->g2h_len == 0 && f->h2g_len == 0)
        flow_free(ns, f);
}

static void handle_tcp(NetStack *ns, const uint8_t *frame, uint32_t ipoff, uint32_t ihl,
                       uint32_t iptotal) {
    if (!ns->io || iptotal < ihl + 20) return;
    const uint8_t *ip = frame + ipoff;
    const uint8_t *t = frame + ipoff + ihl;
    uint16_t sport = be16(t), dport = be16(t + 2);
    uint32_t seq = be32(t + 4), ack = be32(t + 8);
    uint32_t doff = (uint32_t)(t[12] >> 4) * 4u;
    uint8_t flags = t[13];
    uint16_t wnd = be16(t + 14);
    if (doff < 20 || ihl + doff > iptotal) return;
    const uint8_t *payload = t + doff;
    uint32_t plen = iptotal - ihl - doff;

    NatFlow *f = flow_find(ns, NS_PROTO_TCP, sport, ip + 16, dport);

    if (flags & TCP_SYN && !f) {                 /* new connection */
        f = flow_alloc(ns, NS_PROTO_TCP, sport, ip + 16, dport);
        if (!f) return;
        ns->tcp_isn += 0x1000;
        f->snd_una = f->snd_nxt = ns->tcp_isn;   /* our ISN */
        f->rcv_nxt = seq + 1;                    /* past the guest's SYN */
        f->guest_wnd = wnd ? wnd : 1;
        if (ns->io->open(ns->io->ctx, flow_id(ns, f), NS_PROTO_TCP, f->real_ip,
                         dport) != 0) {
            tcp_out(ns, f, (uint8_t)(TCP_RST | TCP_ACK), NULL, 0);
            flow_free(ns, f);
        }
        return;                                  /* SYN-ACK waits for host_connected */
    }
    if (!f) {                                    /* stray segment: reset the guest */
        return;
    }
    if (flags & TCP_RST) { flow_free(ns, f); return; }

    f->guest_wnd = wnd ? wnd : 1;
    if (flags & TCP_ACK) {                       /* advance our unacked boundary */
        if (ack - f->snd_una <= f->snd_nxt - f->snd_una) f->snd_una = ack;
    }
    if (f->state == TS_SYNRCVD && (flags & TCP_ACK) && f->snd_una == f->snd_nxt)
        f->state = TS_ESTAB;                     /* handshake complete */

    if (plen > 0 && f->state == TS_ESTAB && seq == f->rcv_nxt && !f->guest_fin) {
        uint32_t room = NS_TCP_BUF - f->g2h_len;
        uint32_t take = plen < room ? plen : room;
        if (take > 0) {
            if (!f->g2h) f->g2h = malloc(NS_TCP_BUF);
            if (f->g2h) {
                memcpy(f->g2h + f->g2h_len, payload, take);
                f->g2h_len += take;
                f->rcv_nxt += take;              /* we own these bytes now */
            }
        }
        if (!tcp_drain_g2h(ns, f)) return;       /* drain hit a fatal error */
        tcp_out(ns, f, TCP_ACK, NULL, 0);        /* acknowledge what we took */
    } else if (plen > 0 && seq != f->rcv_nxt) {
        tcp_out(ns, f, TCP_ACK, NULL, 0);        /* duplicate/gap: re-ack */
    }

    if ((flags & TCP_FIN) && !f->guest_fin && seq + plen == f->rcv_nxt) {
        f->guest_fin = 1;
        f->rcv_nxt += 1;                         /* FIN consumes a sequence number */
        tcp_out(ns, f, TCP_ACK, NULL, 0);
        f->shutdown_pending = 1;
        if (!tcp_drain_g2h(ns, f)) return;       /* shutdown host send once drained */
    }

    tcp_flush_h2g(ns, f);
    tcp_maybe_close(ns, f);
}

/* ---- IPv4 dispatch. ---- */
static void handle_udp(NetStack *ns, const uint8_t *frame, uint32_t ipoff,
                       uint32_t ihl, uint32_t iptotal) {
    if (iptotal < ihl + 8) return;
    const uint8_t *ip = frame + ipoff;
    const uint8_t *udp = frame + ipoff + ihl;
    if (be16(udp + 2) == 67 && be16(udp) == 68)   /* dst 67, src 68: DHCP */
        handle_dhcp(ns, frame, ipoff + ihl, iptotal - ihl);
    else if (memcmp(ip + 16, gw_ip, 4) != 0)      /* anything not aimed at the gateway */
        udp_nat(ns, frame, ipoff, ihl, iptotal);
}

static void handle_ip(NetStack *ns, const uint8_t *frame, uint32_t len) {
    if (len < ETH_HDR + 20) return;
    const uint8_t *ip = frame + ETH_HDR;
    if ((ip[0] >> 4) != 4) return;                /* IPv4 only */
    uint32_t ihl = (uint32_t)(ip[0] & 0x0f) * 4u;
    if (ihl < 20 || len < ETH_HDR + ihl) return;
    uint32_t iptotal = be16(ip + 2);              /* header + payload per the header */
    uint32_t avail = len - ETH_HDR;               /* what the frame actually carries */
    if (iptotal > avail) iptotal = avail;         /* trust the frame, not the header */
    if (iptotal < ihl) return;

    if (ip[9] == PROTO_ICMP)     handle_icmp(ns, frame, ETH_HDR, ihl, iptotal);
    else if (ip[9] == PROTO_UDP) handle_udp(ns, frame, ETH_HDR, ihl, iptotal);
    else if (ip[9] == PROTO_TCP && memcmp(ip + 16, gw_ip, 4) != 0)
        handle_tcp(ns, frame, ETH_HDR, ihl, iptotal);
}

/* ---- host-side events (called by the NetIo backend's poll loop) ---- */

/* Look up a flow by id, ignoring an out-of-range or freed id. */
static NatFlow *flow_by_id(NetStack *ns, int id) {
    if (id < 0 || id >= NS_NAT_MAX || ns->flows[id].state == TS_FREE) return NULL;
    return &ns->flows[id];
}

void netstack_host_connected(NetStack *ns, int id) {
    NatFlow *f = flow_by_id(ns, id);
    if (!f || f->proto != NS_PROTO_TCP || f->state != TS_CONNECTING) return;
    tcp_synack(ns, f);
    f->snd_nxt += 1;                              /* SYN consumes a sequence number */
    f->state = TS_SYNRCVD;
}

void netstack_host_recv(NetStack *ns, int id, const uint8_t *data, uint32_t len) {
    NatFlow *f = flow_by_id(ns, id);
    if (!f || len == 0) return;
    if (f->proto == NS_PROTO_UDP) { udp_to_guest(ns, f, data, len); return; }
    /* TCP: buffer the bytes and flush within the guest's window. rx_room caps
     * the read, so this always fits. */
    uint32_t room = NS_TCP_BUF - f->h2g_len;
    if (len > room) len = room;
    if (len == 0) return;
    if (!f->h2g) f->h2g = malloc(NS_TCP_BUF);
    if (!f->h2g) return;
    memcpy(f->h2g + f->h2g_len, data, len);
    f->h2g_len += len;
    tcp_flush_h2g(ns, f);
}

void netstack_host_closed(NetStack *ns, int id) {
    NatFlow *f = flow_by_id(ns, id);
    if (!f) return;
    if (f->proto == NS_PROTO_UDP) { flow_free(ns, f); return; }
    f->host_eof = 1;
    tcp_flush_h2g(ns, f);                          /* sends our FIN once drained */
    tcp_maybe_close(ns, f);
}

void netstack_host_reset(NetStack *ns, int id) {
    NatFlow *f = flow_by_id(ns, id);
    if (!f) return;
    if (f->proto == NS_PROTO_TCP && !f->fin_sent)
        tcp_out(ns, f, (uint8_t)(TCP_RST | TCP_ACK), NULL, 0);
    flow_free(ns, f);
}

void netstack_host_writable(NetStack *ns, int id) {
    NatFlow *f = flow_by_id(ns, id);
    if (!f || f->proto != NS_PROTO_TCP) return;
    uint32_t before = f->g2h_len;
    if (!tcp_drain_g2h(ns, f)) return;            /* torn down by a send error */
    if (f->g2h_len < before) tcp_out(ns, f, TCP_ACK, NULL, 0); /* window reopened */
    tcp_maybe_close(ns, f);
}

uint32_t netstack_rx_room(NetStack *ns, int id) {
    NatFlow *f = flow_by_id(ns, id);
    if (!f) return 0;
    if (f->proto == NS_PROTO_UDP) return NS_MTU;  /* one datagram */
    if (f->host_eof || f->state < TS_SYNRCVD) return 0;
    return NS_TCP_BUF - f->h2g_len;
}

int netstack_wants_write(NetStack *ns, int id) {
    NatFlow *f = flow_by_id(ns, id);
    return f && f->proto == NS_PROTO_TCP && f->g2h_len > 0;
}

/* ---- lifecycle ---- */

void netstack_set_io(NetStack *ns, const NetIo *io, const uint8_t dns[4]) {
    if (!ns) return;
    ns->io = io;
    if (dns) memcpy(ns->upstream_dns, dns, 4);
}

NetStack *netstack_new(void (*out)(void *ctx, const uint8_t *frame, uint32_t len),
                       void *ctx) {
    NetStack *ns = calloc(1, sizeof *ns);
    if (!ns) return NULL;
    ns->out = out;
    ns->ctx = ctx;
    ns->ip_id = 1;
    ns->tcp_isn = 0x8000;
    memcpy(ns->upstream_dns, dns_ip, 4);          /* until set_io overrides it */
    return ns;
}

void netstack_free(NetStack *ns) {
    if (!ns) return;
    for (int i = 0; i < NS_NAT_MAX; i++)
        if (ns->flows[i].state != TS_FREE) flow_free(ns, &ns->flows[i]);
    free(ns);
}

void netstack_input(NetStack *ns, const uint8_t *frame, uint32_t len) {
    if (!ns || !ns->out || len < ETH_HDR) return;
    memcpy(ns->guest_mac, frame + ETH_SRC, 6);    /* learn the guest's MAC */
    ns->have_guest_mac = 1;
    uint16_t etype = be16(frame + ETH_TYPE);
    if (etype == ETYPE_ARP)     handle_arp(ns, frame, len);
    else if (etype == ETYPE_IP) handle_ip(ns, frame, len);
}
