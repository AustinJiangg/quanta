#include "netstack.h"

#include <stdlib.h>
#include <string.h>

/*
 * The usermode network stack — see netstack.h for the overview. This is a pure
 * ethernet-frame processor: netstack_input parses a guest frame and, for the
 * protocols the virtual gateway answers (ARP, ICMP echo, DHCP), builds a reply
 * frame and emits it through the out callback. Everything is bounds-checked
 * against the frame length, since the frames are guest-controlled.
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
#define PROTO_UDP  17u

/* The virtual network's fixed addresses (from netstack.h). */
static const uint8_t gw_mac[6]     = NS_GATEWAY_MAC;
static const uint8_t gw_ip[4]      = NS_GATEWAY_IP;
static const uint8_t dns_ip[4]     = NS_DNS_IP;
static const uint8_t guest_ip[4]   = NS_GUEST_IP;
static const uint8_t netmask[4]    = NS_NETMASK;
static const uint8_t dhcp_magic[4] = { 0x63, 0x82, 0x53, 0x63 };

struct NetStack {
    void   (*out)(void *ctx, const uint8_t *frame, uint32_t len);
    void    *ctx;
    uint8_t  guest_mac[6];   /* learned from the guest's frames (for future unicast) */
    int      have_guest_mac;
    uint16_t ip_id;          /* IP identification counter for the frames we emit */
};

/* Big-endian (network byte order) field access. */
static uint16_t be16(const uint8_t *p) { return (uint16_t)((uint32_t)p[0] << 8 | p[1]); }
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

/* ---- IPv4 dispatch. ---- */
static void handle_udp(NetStack *ns, const uint8_t *frame, uint32_t ipoff,
                       uint32_t ihl, uint32_t iptotal) {
    if (iptotal < ihl + 8) return;
    const uint8_t *udp = frame + ipoff + ihl;
    if (be16(udp + 2) == 67 && be16(udp) == 68)   /* dst 67, src 68: DHCP */
        handle_dhcp(ns, frame, ipoff + ihl, iptotal - ihl);
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
}

NetStack *netstack_new(void (*out)(void *ctx, const uint8_t *frame, uint32_t len),
                       void *ctx) {
    NetStack *ns = calloc(1, sizeof *ns);
    if (!ns) return NULL;
    ns->out = out;
    ns->ctx = ctx;
    ns->ip_id = 1;
    return ns;
}

void netstack_free(NetStack *ns) { free(ns); }

void netstack_input(NetStack *ns, const uint8_t *frame, uint32_t len) {
    if (!ns || !ns->out || len < ETH_HDR) return;
    memcpy(ns->guest_mac, frame + ETH_SRC, 6);    /* learn the guest's MAC */
    ns->have_guest_mac = 1;
    uint16_t etype = be16(frame + ETH_TYPE);
    if (etype == ETYPE_ARP)     handle_arp(ns, frame, len);
    else if (etype == ETYPE_IP) handle_ip(ns, frame, len);
}
