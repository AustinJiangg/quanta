/*
 * Unit test for the usermode network stack (M23), linked against libquanta.
 *
 * The stack is a pure ethernet-frame processor, so it is tested in isolation:
 * feed it a hand-built request frame, capture the reply it emits, and check the
 * headers, addresses, and checksums. Covers ARP (answered and ignored), ICMP echo
 * to the gateway, and DHCP DISCOVER->OFFER / REQUEST->ACK. Deterministic and
 * host-independent (no sockets), so it runs everywhere `make check` does.
 *
 * Exits 0 if every check passes, 1 otherwise (with the failures printed).
 */
#include "netstack.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- the virtual network's addresses (from netstack.h) ---- */
static const uint8_t GW_IP[4]    = NS_GATEWAY_IP;
static const uint8_t DNS_IP[4]   = NS_DNS_IP;
static const uint8_t GUEST_IP[4] = NS_GUEST_IP;
static const uint8_t MASK[4]     = NS_NETMASK;
static const uint8_t GW_MAC[6]   = NS_GATEWAY_MAC;
static const uint8_t GUEST_MAC[6] = { 0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc };

/* ---- capture the frames the stack emits ---- */
#define CAP_MAX 8
static uint8_t  cap_frame[CAP_MAX][1600];
static uint32_t cap_len[CAP_MAX];
static int      cap_n;

static void cap_out(void *ctx, const uint8_t *f, uint32_t len) {
    (void)ctx;
    if (cap_n < CAP_MAX && len <= sizeof cap_frame[0]) {
        memcpy(cap_frame[cap_n], f, len);
        cap_len[cap_n] = len;
    }
    cap_n++;
}
static void cap_reset(void) { cap_n = 0; }

/* ---- byte helpers ---- */
static uint16_t be16(const uint8_t *p) { return (uint16_t)((uint32_t)p[0] << 8 | p[1]); }
static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint16_t inet_csum(const uint8_t *d, uint32_t len) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i + 1 < len; i += 2) sum += (uint32_t)d[i] << 8 | d[i + 1];
    if (len & 1) sum += (uint32_t)d[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } \
} while (0)

/* Find a DHCP option in a reply message (options start at offset 240). */
static const uint8_t *dhcp_opt(const uint8_t *d, uint32_t len, uint8_t code, uint8_t *olen) {
    uint32_t i = 240;
    while (i < len) {
        uint8_t c = d[i++];
        if (c == 0) continue;
        if (c == 255) break;
        if (i >= len) break;
        uint8_t l = d[i++];
        if (i + l > len) break;
        if (c == code) { if (olen) *olen = l; return d + i; }
        i += l;
    }
    return NULL;
}

/* ---- 1. ARP request for the gateway -> reply with the gateway MAC ---- */
static void test_arp(NetStack *ns) {
    uint8_t f[42];
    memset(f, 0, sizeof f);
    memset(f + 0, 0xff, 6);              /* eth dst = broadcast */
    memcpy(f + 6, GUEST_MAC, 6);         /* eth src = guest */
    put_be16(f + 12, 0x0806);            /* ARP */
    uint8_t *a = f + 14;
    put_be16(a, 1); put_be16(a + 2, 0x0800); a[4] = 6; a[5] = 4;
    put_be16(a + 6, 1);                  /* request */
    memcpy(a + 8, GUEST_MAC, 6);         /* sha */
    memcpy(a + 14, GUEST_IP, 4);         /* spa */
    memcpy(a + 24, GW_IP, 4);            /* tpa = gateway */

    cap_reset();
    netstack_input(ns, f, sizeof f);
    CHECK(cap_n == 1, "arp: exactly one reply");
    if (cap_n != 1) return;
    const uint8_t *r = cap_frame[0];
    CHECK(cap_len[0] == 42, "arp: reply length");
    CHECK(memcmp(r + 0, GUEST_MAC, 6) == 0, "arp: eth dst = requester");
    CHECK(memcmp(r + 6, GW_MAC, 6) == 0, "arp: eth src = gateway");
    CHECK(be16(r + 12) == 0x0806, "arp: ethertype");
    CHECK(be16(r + 14 + 6) == 2, "arp: oper = reply");
    CHECK(memcmp(r + 14 + 8, GW_MAC, 6) == 0, "arp: sha = gateway mac");
    CHECK(memcmp(r + 14 + 14, GW_IP, 4) == 0, "arp: spa = gateway ip");
    CHECK(memcmp(r + 14 + 18, GUEST_MAC, 6) == 0, "arp: tha = requester");
    CHECK(memcmp(r + 14 + 24, GUEST_IP, 4) == 0, "arp: tpa = requester ip");
}

/* ---- 2. ARP for an address we do not own -> ignored ---- */
static void test_arp_ignored(NetStack *ns) {
    uint8_t f[42];
    memset(f, 0, sizeof f);
    memset(f + 0, 0xff, 6);
    memcpy(f + 6, GUEST_MAC, 6);
    put_be16(f + 12, 0x0806);
    uint8_t *a = f + 14;
    put_be16(a, 1); put_be16(a + 2, 0x0800); a[4] = 6; a[5] = 4;
    put_be16(a + 6, 1);
    memcpy(a + 8, GUEST_MAC, 6);
    memcpy(a + 14, GUEST_IP, 4);
    uint8_t other[4] = { 10, 0, 2, 99 };
    memcpy(a + 24, other, 4);            /* tpa = unowned host */

    cap_reset();
    netstack_input(ns, f, sizeof f);
    CHECK(cap_n == 0, "arp: unowned target ignored");
}

/* ---- 3. ICMP echo to the gateway -> echo reply ---- */
static void test_icmp(NetStack *ns) {
    uint8_t f[14 + 20 + 16];
    memset(f, 0, sizeof f);
    memcpy(f + 0, GW_MAC, 6);            /* eth dst = gateway */
    memcpy(f + 6, GUEST_MAC, 6);
    put_be16(f + 12, 0x0800);
    uint8_t *ip = f + 14;
    ip[0] = 0x45; put_be16(ip + 2, 20 + 16);
    ip[8] = 64; ip[9] = 1;              /* ttl, proto ICMP */
    memcpy(ip + 12, GUEST_IP, 4);
    memcpy(ip + 16, GW_IP, 4);
    put_be16(ip + 10, inet_csum(ip, 20));
    uint8_t *ic = ip + 20;
    ic[0] = 8; ic[1] = 0;               /* echo request */
    put_be16(ic + 4, 0x1234);          /* id */
    put_be16(ic + 6, 1);               /* seq */
    memcpy(ic + 8, "abcdefgh", 8);     /* payload */
    put_be16(ic + 2, inet_csum(ic, 16));

    cap_reset();
    netstack_input(ns, f, sizeof f);
    CHECK(cap_n == 1, "icmp: exactly one reply");
    if (cap_n != 1) return;
    const uint8_t *r = cap_frame[0];
    CHECK(memcmp(r + 0, GUEST_MAC, 6) == 0, "icmp: eth dst = sender");
    CHECK(memcmp(r + 6, GW_MAC, 6) == 0, "icmp: eth src = gateway");
    const uint8_t *rip = r + 14;
    CHECK(memcmp(rip + 12, GW_IP, 4) == 0, "icmp: ip src = gateway");
    CHECK(memcmp(rip + 16, GUEST_IP, 4) == 0, "icmp: ip dst = sender");
    CHECK(rip[9] == 1, "icmp: proto");
    CHECK(inet_csum(rip, 20) == 0, "icmp: ip checksum valid");
    const uint8_t *ric = rip + 20;
    CHECK(ric[0] == 0, "icmp: type = echo reply");
    CHECK(be16(ric + 4) == 0x1234, "icmp: id echoed");
    CHECK(be16(ric + 6) == 1, "icmp: seq echoed");
    CHECK(memcmp(ric + 8, "abcdefgh", 8) == 0, "icmp: payload echoed");
    CHECK(inet_csum(ric, 16) == 0, "icmp: icmp checksum valid");
}

/* Build a DHCP request (DISCOVER if msgtype 1, REQUEST if 3) into f, return len. */
static uint32_t build_dhcp(uint8_t *f, uint8_t msgtype) {
    uint32_t dhcp_msg = 244;             /* 240 fixed+cookie + 4 option bytes */
    uint32_t udp_len = 8 + dhcp_msg;
    uint32_t ip_len = 20 + udp_len;
    memset(f, 0, 14 + ip_len);
    memset(f + 0, 0xff, 6);
    memcpy(f + 6, GUEST_MAC, 6);
    put_be16(f + 12, 0x0800);
    uint8_t *ip = f + 14;
    ip[0] = 0x45; put_be16(ip + 2, (uint16_t)ip_len);
    ip[8] = 64; ip[9] = 17;
    memset(ip + 16, 0xff, 4);            /* dst = broadcast */
    put_be16(ip + 10, inet_csum(ip, 20));
    uint8_t *udp = ip + 20;
    put_be16(udp, 68); put_be16(udp + 2, 67);
    put_be16(udp + 4, (uint16_t)udp_len);
    uint8_t *d = udp + 8;
    d[0] = 1; d[1] = 1; d[2] = 6;        /* BOOTREQUEST */
    put_be32(d + 4, 0x12345678);         /* xid */
    memcpy(d + 28, GUEST_MAC, 6);        /* chaddr */
    uint8_t cookie[4] = { 0x63, 0x82, 0x53, 0x63 };
    memcpy(d + 236, cookie, 4);
    d[240] = 53; d[241] = 1; d[242] = msgtype; d[243] = 255;
    return 14 + ip_len;
}

/* ---- 4/5. DHCP DISCOVER->OFFER (msgtype 1->2) and REQUEST->ACK (3->5) ---- */
static void test_dhcp(NetStack *ns, uint8_t req, uint8_t want, const char *label) {
    uint8_t f[400];
    uint32_t len = build_dhcp(f, req);

    cap_reset();
    netstack_input(ns, f, len);
    CHECK(cap_n == 1, label);
    if (cap_n != 1) return;
    const uint8_t *r = cap_frame[0];
    const uint8_t *rip = r + 14;
    CHECK(memcmp(rip + 12, GW_IP, 4) == 0, "dhcp: ip src = server");
    CHECK(inet_csum(rip, 20) == 0, "dhcp: ip checksum valid");
    const uint8_t *udp = rip + 20;
    CHECK(be16(udp) == 67 && be16(udp + 2) == 68, "dhcp: udp ports");
    const uint8_t *d = udp + 8;
    uint32_t dlen = cap_len[0] - 14 - 20 - 8;
    CHECK(d[0] == 2, "dhcp: op = BOOTREPLY");
    CHECK(be16(d + 4) == 0x1234 && be16(d + 6) == 0x5678, "dhcp: xid echoed");
    CHECK(memcmp(d + 16, GUEST_IP, 4) == 0, "dhcp: yiaddr = offered address");
    CHECK(memcmp(d + 28, GUEST_MAC, 6) == 0, "dhcp: chaddr = client");
    uint8_t ol = 0;
    const uint8_t *o = dhcp_opt(d, dlen, 53, &ol);
    CHECK(o && ol == 1 && o[0] == want, "dhcp: message type");
    o = dhcp_opt(d, dlen, 54, &ol);
    CHECK(o && ol == 4 && memcmp(o, GW_IP, 4) == 0, "dhcp: server id");
    o = dhcp_opt(d, dlen, 1, &ol);
    CHECK(o && ol == 4 && memcmp(o, MASK, 4) == 0, "dhcp: subnet mask");
    o = dhcp_opt(d, dlen, 3, &ol);
    CHECK(o && ol == 4 && memcmp(o, GW_IP, 4) == 0, "dhcp: router");
    o = dhcp_opt(d, dlen, 6, &ol);
    CHECK(o && ol == 4 && memcmp(o, DNS_IP, 4) == 0, "dhcp: dns");
}

int main(void) {
    NetStack *ns = netstack_new(cap_out, NULL);
    if (!ns) { fprintf(stderr, "netstack_new failed\n"); return 1; }

    test_arp(ns);
    test_arp_ignored(ns);
    test_icmp(ns);
    test_dhcp(ns, 1, 2, "dhcp: discover -> offer");
    test_dhcp(ns, 3, 5, "dhcp: request -> ack");

    netstack_free(ns);

    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    printf("all netstack checks passed\n");
    return 0;
}
