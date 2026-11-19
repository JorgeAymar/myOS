// UDP: port bindings with callbacks, checksummed send/receive. Owner: net agent.
#include "net_internal.h"
#include "arch.h"
#include "string.h"

#define UDP_HLEN  8
#define UDP_BINDS 16

static struct { uint16_t port; udp_handler_t fn; } binds[UDP_BINDS];
static uint16_t next_port;

uint16_t net_ephemeral_port(void) {
    if (!next_port) next_port = (uint16_t)(49152 + (timer_ticks_ms() * 7919) % 16000);
    uint16_t p = next_port++;
    if (next_port < 49152) next_port = 49152;
    return p;
}

bool udp_bind(uint16_t port, udp_handler_t fn) {
    int free_slot = -1;
    for (int i = 0; i < UDP_BINDS; i++) {
        if (binds[i].fn && binds[i].port == port) return false;
        if (!binds[i].fn && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return false;
    binds[free_slot].port = port;
    binds[free_slot].fn = fn;
    return true;
}

void udp_unbind(uint16_t port) {
    for (int i = 0; i < UDP_BINDS; i++)
        if (binds[i].fn && binds[i].port == port) binds[i].fn = 0;
}

int udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const void *data, size_t len) {
    if (len > ETH_MTU - IP_HLEN - UDP_HLEN) return NET_ERR;
    uint8_t seg[ETH_MTU - IP_HLEN];
    uint16_t ulen = (uint16_t)(UDP_HLEN + len);
    wr16(seg, sport);
    wr16(seg + 2, dport);
    wr16(seg + 4, ulen);
    wr16(seg + 6, 0);
    memcpy(seg + UDP_HLEN, data, len);
    uint16_t c = csum_fold(csum_add(csum_pseudo(net_if.ip, dst, IPPROTO_UDP, ulen), seg, ulen));
    wr16(seg + 6, c ? c : 0xffff);
    return ip_send(dst, IPPROTO_UDP, seg, ulen);
}

void udp_input(uint32_t src, uint32_t dst, const uint8_t *data, size_t len) {
    if (len < UDP_HLEN) return;
    uint16_t ulen = rd16(data + 4);
    if (ulen < UDP_HLEN || ulen > len) return;
    if (rd16(data + 6) != 0 &&
        csum_fold(csum_add(csum_pseudo(src, dst, IPPROTO_UDP, ulen), data, ulen)) != 0) return;
    uint16_t sport = rd16(data), dport = rd16(data + 2);
    for (int i = 0; i < UDP_BINDS; i++) {
        if (binds[i].fn && binds[i].port == dport) {
            binds[i].fn(src, sport, data + UDP_HLEN, ulen - UDP_HLEN);
            return;
        }
    }
}
