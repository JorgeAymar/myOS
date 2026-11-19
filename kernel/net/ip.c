// IPv4 (send/receive, header checksum, no fragmentation) and ICMP echo
// (replies + ping client). Owner: net agent.
#include "net_internal.h"
#include "arch.h"
#include "string.h"

static uint16_t ip_ident;

uint32_t csum_add(uint32_t sum, const void *data, size_t len) {
    const uint8_t *p = data;
    while (len > 1) { sum += (uint32_t)((p[0] << 8) | p[1]); p += 2; len -= 2; }
    if (len) sum += (uint32_t)(p[0] << 8);
    return sum;
}

uint16_t csum_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

uint32_t csum_pseudo(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len) {
    return (src >> 16) + (src & 0xffff) + (dst >> 16) + (dst & 0xffff) + proto + len;
}

static bool is_broadcast(uint32_t ip) {
    return ip == 0xffffffff || (net_if.mask && (ip & ~net_if.mask) == ~net_if.mask &&
                                (ip & net_if.mask) == (net_if.ip & net_if.mask));
}

int ip_send(uint32_t dst, uint8_t proto, const void *payload, size_t len) {
    if (!net_if.up) return NET_ERR;
    if (len > ETH_MTU - IP_HLEN) return NET_ERR;
    uint8_t mac[6];
    if (is_broadcast(dst)) {
        memcpy(mac, eth_broadcast, 6);
    } else {
        uint32_t hop = dst;
        if (net_if.mask && (dst & net_if.mask) != (net_if.ip & net_if.mask)) hop = net_if.gw;
        if (!hop) return NET_ERR;
        if (!arp_lookup(hop, mac)) {
            // Inside the receive path we can't block: ask and drop (TCP retransmits).
            if (net_in_poll) { arp_request(hop); return NET_ETIMEDOUT; }
            if (!arp_resolve(hop, mac, 1000)) return NET_ETIMEDOUT;
        }
    }
    uint8_t pkt[ETH_MTU];
    uint8_t *h = pkt;
    h[0] = 0x45; h[1] = 0;
    wr16(h + 2, (uint16_t)(IP_HLEN + len));
    wr16(h + 4, ip_ident++);
    wr16(h + 6, 0x4000);            // don't fragment
    h[8] = 64; h[9] = proto;
    wr16(h + 10, 0);
    wr32(h + 12, net_if.ip);
    wr32(h + 16, dst);
    wr16(h + 10, csum_fold(csum_add(0, h, IP_HLEN)));
    memcpy(pkt + IP_HLEN, payload, len);
    return eth_send(mac, ETHERTYPE_IP, pkt, IP_HLEN + len) ? 0 : NET_ERR;
}

void ip_input(const uint8_t *pkt, size_t len) {
    if (len < IP_HLEN || (pkt[0] >> 4) != 4) return;
    size_t hlen = (size_t)(pkt[0] & 0xf) * 4;
    size_t tlen = rd16(pkt + 2);
    if (hlen < IP_HLEN || tlen < hlen || tlen > len) return;
    if (csum_fold(csum_add(0, pkt, hlen)) != 0) return;
    if (rd16(pkt + 6) & 0x3fff) return;          // fragments: not supported
    uint32_t src = rd32(pkt + 12), dst = rd32(pkt + 16);
    // Before DHCP completes accept anything (offers may be unicast to yiaddr).
    if (net_if.ip && dst != net_if.ip && !is_broadcast(dst)) return;
    const uint8_t *data = pkt + hlen;
    size_t dlen = tlen - hlen;
    switch (pkt[9]) {
    case IPPROTO_ICMP: icmp_input(src, data, dlen); break;
    case IPPROTO_UDP:  udp_input(src, dst, data, dlen); break;
    case IPPROTO_TCP:  tcp_input(src, dst, data, dlen); break;
    default: break;
    }
}

// ---- ICMP ----
#define PING_ID 0x6d79   // "my"
static uint16_t ping_seq;
static volatile bool ping_got;
static uint32_t ping_from;

void icmp_input(uint32_t src, const uint8_t *data, size_t len) {
    if (len < 8 || csum_fold(csum_add(0, data, len)) != 0) return;
    if (data[0] == 8 && net_if.ip) {             // echo request -> reply
        if (len > ETH_MTU - IP_HLEN) return;
        uint8_t reply[ETH_MTU - IP_HLEN];
        memcpy(reply, data, len);
        reply[0] = 0;
        wr16(reply + 2, 0);
        wr16(reply + 2, csum_fold(csum_add(0, reply, len)));
        ip_send(src, IPPROTO_ICMP, reply, len);
    } else if (data[0] == 0) {                   // echo reply
        if (rd16(data + 4) == PING_ID && rd16(data + 6) == ping_seq) {
            ping_from = src;
            ping_got = true;
        }
    }
}

int net_ping(uint32_t ip, uint32_t timeout_ms) {
    if (!net_if.up || !net_if.ip) return NET_ERR;
    uint8_t msg[8 + 32];
    msg[0] = 8; msg[1] = 0;
    wr16(msg + 2, 0);
    wr16(msg + 4, PING_ID);
    wr16(msg + 6, ++ping_seq);
    for (size_t i = 8; i < sizeof msg; i++) msg[i] = (uint8_t)('a' + (i - 8) % 26);
    wr16(msg + 2, csum_fold(csum_add(0, msg, sizeof msg)));
    ping_got = false;
    uint64_t start = timer_ticks_ms();
    int r = ip_send(ip, IPPROTO_ICMP, msg, sizeof msg);
    if (r < 0) return r;
    uint64_t left = timer_ticks_ms() - start;
    left = left >= timeout_ms ? 0 : timeout_ms - left;
    if (!NET_WAIT(ping_got && ping_from == ip, left)) return NET_ETIMEDOUT;
    return (int)(timer_ticks_ms() - start);
}
