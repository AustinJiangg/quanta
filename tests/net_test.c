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
static const uint8_t REMOTE[4]   = { 93, 184, 216, 34 };   /* an off-network host */
static const uint8_t UPSTREAM[4] = { 1, 1, 1, 1 };         /* the real DNS resolver */

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
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
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

/* ======================================================================
 * Outbound NAT (UDP/TCP + DNS relay). These drive the stack against a mock
 * NetIo backend that records what the stack asks of the host and lets the test
 * inject host-side events, so the NAT protocol logic is exercised deterministically
 * with no real sockets. A fresh stack per test makes the flow id predictably 0.
 * ==================================================================== */

/* TCP flag bits (netstack.c keeps its own copy; mirrored here for the frames). */
#define T_FIN 0x01u
#define T_SYN 0x02u
#define T_RST 0x04u
#define T_ACK 0x10u

static struct MockSock {
    int used, proto, shutdown_called, closed;
    uint8_t ip[4]; uint16_t port;
    uint8_t sent[4096]; uint32_t sent_len;
} mock[NS_NAT_MAX];
static int mock_open_fail;

static int mio_open(void *c, int id, int proto, const uint8_t ip[4], uint16_t port) {
    (void)c;
    if (mock_open_fail || id < 0 || id >= NS_NAT_MAX) return -1;
    mock[id].used = 1; mock[id].proto = proto; mock[id].sent_len = 0;
    mock[id].shutdown_called = 0; mock[id].closed = 0;
    memcpy(mock[id].ip, ip, 4); mock[id].port = port;
    return 0;
}
static int mio_send(void *c, int id, const uint8_t *d, uint32_t len) {
    (void)c;
    if (mock[id].sent_len + len > sizeof mock[id].sent)
        len = (uint32_t)sizeof mock[id].sent - mock[id].sent_len;
    memcpy(mock[id].sent + mock[id].sent_len, d, len);
    mock[id].sent_len += len;
    return (int)len;
}
static void mio_shutdown(void *c, int id) { (void)c; mock[id].shutdown_called = 1; }
static void mio_close(void *c, int id) { (void)c; mock[id].used = 0; mock[id].closed = 1; }
static const NetIo MIO = { NULL, mio_open, mio_send, mio_shutdown, mio_close };

/* The TCP/UDP pseudo-header checksum, mirroring netstack.c. */
static uint16_t l4_csum(const uint8_t *src, const uint8_t *dst, uint8_t proto,
                        const uint8_t *l4, uint32_t l4len) {
    uint32_t sum = 0;
    sum += (uint32_t)src[0] << 8 | src[1]; sum += (uint32_t)src[2] << 8 | src[3];
    sum += (uint32_t)dst[0] << 8 | dst[1]; sum += (uint32_t)dst[2] << 8 | dst[3];
    sum += proto; sum += l4len;
    for (uint32_t i = 0; i + 1 < l4len; i += 2) sum += (uint32_t)l4[i] << 8 | l4[i + 1];
    if (l4len & 1) sum += (uint32_t)l4[l4len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Wrap an L4 payload in a guest->stack IPv4 frame to `dip`. eth.dst is the
 * gateway (the guest's default route for an off-subnet address). */
static uint32_t build_ip(uint8_t *f, uint8_t proto, const uint8_t *dip,
                         const uint8_t *l4, uint32_t l4len) {
    memcpy(f + 0, GW_MAC, 6);
    memcpy(f + 6, GUEST_MAC, 6);
    put_be16(f + 12, 0x0800);
    uint8_t *ip = f + 14;
    uint32_t iptot = 20 + l4len;
    memset(ip, 0, 20);
    ip[0] = 0x45; put_be16(ip + 2, (uint16_t)iptot);
    ip[8] = 64; ip[9] = proto;
    memcpy(ip + 12, GUEST_IP, 4);
    memcpy(ip + 16, dip, 4);
    put_be16(ip + 10, inet_csum(ip, 20));
    memcpy(ip + 20, l4, l4len);
    return 14 + iptot;
}

static uint32_t build_udp(uint8_t *l4, uint16_t sport, uint16_t dport,
                          const void *pl, uint32_t plen) {
    put_be16(l4, sport); put_be16(l4 + 2, dport);
    put_be16(l4 + 4, (uint16_t)(8 + plen)); put_be16(l4 + 6, 0);
    memcpy(l4 + 8, pl, plen);
    return 8 + plen;
}

static uint32_t build_tcp(uint8_t *l4, const uint8_t *dip, uint16_t sport, uint16_t dport,
                          uint32_t seq, uint32_t ack, uint8_t flags, uint16_t wnd,
                          const void *pl, uint32_t plen) {
    memset(l4, 0, 20);
    put_be16(l4, sport); put_be16(l4 + 2, dport);
    put_be32(l4 + 4, seq); put_be32(l4 + 8, ack);
    l4[12] = 5 << 4; l4[13] = flags; put_be16(l4 + 14, wnd);
    if (plen) memcpy(l4 + 20, pl, plen);
    put_be16(l4 + 16, l4_csum(GUEST_IP, dip, 6, l4, 20 + plen));
    return 20 + plen;
}

/* ---- 6. UDP NAT: guest datagram out, host datagram back ---- */
static void test_udp_nat(void) {
    memset(mock, 0, sizeof mock); mock_open_fail = 0;
    NetStack *ns = netstack_new(cap_out, NULL);
    netstack_set_io(ns, &MIO, UPSTREAM);
    uint8_t f[128], l4[64];
    uint32_t l4n = build_udp(l4, 40000, 5000, "hello", 5);
    cap_reset();
    netstack_input(ns, f, build_ip(f, 17, REMOTE, l4, l4n));
    CHECK(mock[0].used && mock[0].proto == NS_PROTO_UDP, "udp: flow opened");
    CHECK(memcmp(mock[0].ip, REMOTE, 4) == 0 && mock[0].port == 5000, "udp: remote addr");
    CHECK(mock[0].sent_len == 5 && memcmp(mock[0].sent, "hello", 5) == 0, "udp: payload sent");
    CHECK(cap_n == 0, "udp: no immediate reply");

    cap_reset();
    netstack_host_recv(ns, 0, (const uint8_t *)"WORLD!", 6);
    CHECK(cap_n == 1, "udp: one reply frame");
    if (cap_n == 1) {
        const uint8_t *ip = cap_frame[0] + 14, *udp = ip + 20;
        CHECK(memcmp(cap_frame[0], GUEST_MAC, 6) == 0, "udp: eth dst = guest");
        CHECK(memcmp(ip + 12, REMOTE, 4) == 0, "udp: ip src = remote");
        CHECK(memcmp(ip + 16, GUEST_IP, 4) == 0, "udp: ip dst = guest");
        CHECK(inet_csum(ip, 20) == 0, "udp: ip checksum valid");
        CHECK(be16(udp) == 5000 && be16(udp + 2) == 40000, "udp: ports swapped");
        CHECK(memcmp(udp + 8, "WORLD!", 6) == 0, "udp: payload delivered");
    }
    netstack_free(ns);
}

/* ---- 7. DNS relay: query to the virtual DNS server reaches the real resolver,
 *        and the reply appears to come from the virtual server. ---- */
static void test_dns_relay(void) {
    memset(mock, 0, sizeof mock); mock_open_fail = 0;
    NetStack *ns = netstack_new(cap_out, NULL);
    netstack_set_io(ns, &MIO, UPSTREAM);
    uint8_t f[128], l4[64];
    uint32_t l4n = build_udp(l4, 33333, 53, "\x12\x34q", 3);
    cap_reset();
    netstack_input(ns, f, build_ip(f, 17, DNS_IP, l4, l4n));
    CHECK(mock[0].used && memcmp(mock[0].ip, UPSTREAM, 4) == 0 && mock[0].port == 53,
          "dns: relayed to the upstream resolver");

    cap_reset();
    netstack_host_recv(ns, 0, (const uint8_t *)"\x12\x34" "a", 3);
    CHECK(cap_n == 1, "dns: one reply");
    if (cap_n == 1) {
        const uint8_t *ip = cap_frame[0] + 14, *udp = ip + 20;
        CHECK(memcmp(ip + 12, DNS_IP, 4) == 0, "dns: reply src = virtual dns server");
        CHECK(be16(udp) == 53 && be16(udp + 2) == 33333, "dns: ports");
    }
    netstack_free(ns);
}

/* ---- 8. TCP NAT: full connect / bidirectional data / orderly close. ---- */
static void test_tcp_nat(void) {
    memset(mock, 0, sizeof mock); mock_open_fail = 0;
    NetStack *ns = netstack_new(cap_out, NULL);
    netstack_set_io(ns, &MIO, UPSTREAM);
    uint8_t f[256], l4[128];
    uint32_t gseq = 1000; uint16_t wnd = 64240;

    /* SYN -> a connect is started; the SYN-ACK waits for it to complete. */
    uint32_t n = build_tcp(l4, REMOTE, 50000, 80, gseq, 0, (uint8_t)T_SYN, wnd, NULL, 0);
    cap_reset();
    netstack_input(ns, f, build_ip(f, 6, REMOTE, l4, n));
    CHECK(mock[0].used && mock[0].proto == NS_PROTO_TCP, "tcp: connect started");
    CHECK(memcmp(mock[0].ip, REMOTE, 4) == 0 && mock[0].port == 80, "tcp: remote addr");
    CHECK(cap_n == 0, "tcp: no synack before connect completes");

    /* connect completes -> SYN-ACK, acking the guest's SYN. */
    cap_reset();
    netstack_host_connected(ns, 0);
    CHECK(cap_n == 1, "tcp: synack emitted on connect");
    uint32_t isn = 0;
    if (cap_n == 1) {
        const uint8_t *t = cap_frame[0] + 14 + 20;
        CHECK((t[13] & (T_SYN | T_ACK)) == (T_SYN | T_ACK), "tcp: synack flags");
        CHECK(be32(t + 8) == gseq + 1, "tcp: synack acks the guest syn");
        isn = be32(t + 4);
    }

    /* guest ACKs the SYN-ACK -> established (a bare ack draws no reply). */
    n = build_tcp(l4, REMOTE, 50000, 80, gseq + 1, isn + 1, (uint8_t)T_ACK, wnd, NULL, 0);
    cap_reset();
    netstack_input(ns, f, build_ip(f, 6, REMOTE, l4, n));
    CHECK(cap_n == 0, "tcp: bare ack, no reply");

    /* guest sends a request -> forwarded to the host, acknowledged to the guest. */
    const char *req = "GET / HTTP/1.0\r\n\r\n";
    uint32_t rlen = (uint32_t)strlen(req);
    n = build_tcp(l4, REMOTE, 50000, 80, gseq + 1, isn + 1, (uint8_t)T_ACK, wnd, req, rlen);
    cap_reset();
    netstack_input(ns, f, build_ip(f, 6, REMOTE, l4, n));
    CHECK(mock[0].sent_len == rlen && memcmp(mock[0].sent, req, rlen) == 0,
          "tcp: request forwarded to host");
    CHECK(cap_n == 1, "tcp: request acked");
    if (cap_n == 1)
        CHECK(be32(cap_frame[0] + 14 + 20 + 8) == gseq + 1 + rlen, "tcp: ack covers request");

    /* host sends a response -> delivered to the guest as a data segment. */
    const char *resp = "HTTP/1.0 200 OK\r\n";
    uint32_t plen = (uint32_t)strlen(resp);
    cap_reset();
    netstack_host_recv(ns, 0, (const uint8_t *)resp, plen);
    CHECK(cap_n == 1, "tcp: response segment to guest");
    if (cap_n == 1) {
        const uint8_t *t = cap_frame[0] + 14 + 20;
        CHECK(be32(t + 4) == isn + 1, "tcp: response seq");
        CHECK((t[13] & T_ACK) && memcmp(t + 20, resp, plen) == 0, "tcp: response payload");
    }

    /* guest ACKs the response. */
    n = build_tcp(l4, REMOTE, 50000, 80, gseq + 1 + rlen, isn + 1 + plen, (uint8_t)T_ACK,
                  wnd, NULL, 0);
    netstack_input(ns, f, build_ip(f, 6, REMOTE, l4, n));

    /* host closes -> FIN to the guest at the right sequence. */
    cap_reset();
    netstack_host_closed(ns, 0);
    CHECK(cap_n == 1, "tcp: fin to guest on host eof");
    if (cap_n == 1) {
        const uint8_t *t = cap_frame[0] + 14 + 20;
        CHECK((t[13] & T_FIN) != 0, "tcp: fin flag");
        CHECK(be32(t + 4) == isn + 1 + plen, "tcp: fin seq");
    }

    /* guest FIN -> the host send side is shut down and the flow is torn down. */
    n = build_tcp(l4, REMOTE, 50000, 80, gseq + 1 + rlen, isn + 2 + plen,
                  (uint8_t)(T_ACK | T_FIN), wnd, NULL, 0);
    netstack_input(ns, f, build_ip(f, 6, REMOTE, l4, n));
    CHECK(mock[0].shutdown_called, "tcp: host send shut down on guest fin");
    CHECK(mock[0].closed, "tcp: flow torn down after both fins");
    netstack_free(ns);
}

/* ---- 9. A refused connection (open fails) -> the guest gets a reset. ---- */
static void test_tcp_refused(void) {
    memset(mock, 0, sizeof mock); mock_open_fail = 1;
    NetStack *ns = netstack_new(cap_out, NULL);
    netstack_set_io(ns, &MIO, UPSTREAM);
    uint8_t f[256], l4[128];
    uint32_t n = build_tcp(l4, REMOTE, 50001, 80, 5000, 0, (uint8_t)T_SYN, 64240, NULL, 0);
    cap_reset();
    netstack_input(ns, f, build_ip(f, 6, REMOTE, l4, n));
    CHECK(cap_n == 1, "tcp: reset when the connection cannot open");
    if (cap_n == 1)
        CHECK((cap_frame[0][14 + 20 + 13] & T_RST) != 0, "tcp: rst flag");
    netstack_free(ns);
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

    test_udp_nat();
    test_dns_relay();
    test_tcp_nat();
    test_tcp_refused();

    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    printf("all netstack checks passed\n");
    return 0;
}
