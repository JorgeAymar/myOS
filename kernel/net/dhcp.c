// DHCP client (DISCOVER -> OFFER -> REQUEST -> ACK). Owner: net agent.
#include "net_internal.h"
#include "arch.h"
#include "string.h"

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_MAGIC 0x63825363u

enum { DHCPDISCOVER = 1, DHCPOFFER = 2, DHCPREQUEST = 3, DHCPACK = 5, DHCPNAK = 6 };

static uint32_t xid;
static int state;                 // last message type received for our xid
static uint32_t offer_ip, server_id, opt_mask, opt_router, opt_dns;

static void dhcp_handler(uint32_t src, uint16_t sport, const void *data, size_t len) {
    (void)src; (void)sport;
    const uint8_t *p = data;
    if (len < 240 || p[0] != 2 || rd32(p + 4) != xid || rd32(p + 236) != DHCP_MAGIC) return;
    if (memcmp(p + 28, net_if.mac, 6) != 0) return;
    int type = 0;
    uint32_t mask = 0, router = 0, dns = 0, sid = 0;
    size_t i = 240;
    while (i < len) {
        uint8_t opt = p[i++];
        if (opt == 0) continue;
        if (opt == 255 || i >= len) break;
        uint8_t olen = p[i++];
        if (i + olen > len) break;
        const uint8_t *v = p + i;
        switch (opt) {
        case 53: if (olen >= 1) type = v[0]; break;
        case 1:  if (olen >= 4) mask = rd32(v); break;
        case 3:  if (olen >= 4) router = rd32(v); break;
        case 6:  if (olen >= 4) dns = rd32(v); break;
        case 54: if (olen >= 4) sid = rd32(v); break;
        default: break;
        }
        i += olen;
    }
    if (type == DHCPOFFER && state == 0) {
        offer_ip = rd32(p + 16);
        server_id = sid ? sid : src;
        state = DHCPOFFER;
    } else if ((type == DHCPACK || type == DHCPNAK) && state == DHCPOFFER) {
        if (type == DHCPACK) {
            offer_ip = rd32(p + 16);
            opt_mask = mask; opt_router = router; opt_dns = dns;
        }
        state = type;
    }
}

static int dhcp_send(int type) {
    uint8_t m[300];
    memset(m, 0, sizeof m);
    m[0] = 1; m[1] = 1; m[2] = 6;           // BOOTREQUEST, Ethernet, hlen 6
    wr32(m + 4, xid);
    wr16(m + 10, 0x8000);                    // broadcast flag
    memcpy(m + 28, net_if.mac, 6);
    wr32(m + 236, DHCP_MAGIC);
    size_t i = 240;
    m[i++] = 53; m[i++] = 1; m[i++] = (uint8_t)type;
    m[i++] = 61; m[i++] = 7; m[i++] = 1; memcpy(m + i, net_if.mac, 6); i += 6;
    if (type == DHCPREQUEST) {
        m[i++] = 50; m[i++] = 4; wr32(m + i, offer_ip); i += 4;
        m[i++] = 54; m[i++] = 4; wr32(m + i, server_id); i += 4;
    }
    m[i++] = 12; m[i++] = 4; memcpy(m + i, "myos", 4); i += 4;
    m[i++] = 55; m[i++] = 3; m[i++] = 1; m[i++] = 3; m[i++] = 6;
    m[i++] = 255;
    return udp_send(0xffffffff, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, m, sizeof m);
}

bool dhcp_run(uint32_t timeout_ms) {
    if (!udp_bind(DHCP_CLIENT_PORT, dhcp_handler)) return false;
    uint64_t deadline = timer_ticks_ms() + timeout_ms;
    bool ok = false;
    xid = 0x6d790000u ^ (uint32_t)timer_ticks_ms() ^ ((uint32_t)net_if.mac[5] << 8);
    // The first DISCOVER is often lost while the link settles: retry quickly
    // (200, 400, 800, then every 1000 ms) instead of waiting a full second.
    uint64_t wait = 200;
    while (!ok && timer_ticks_ms() < deadline) {
        state = 0;
        xid++;
        dhcp_send(DHCPDISCOVER);
        bool offered = NET_WAIT(state == DHCPOFFER, wait);
        if (wait < 1000) wait = wait * 2 > 1000 ? 1000 : wait * 2;
        if (!offered) continue;
        dhcp_send(DHCPREQUEST);
        if (!NET_WAIT(state == DHCPACK || state == DHCPNAK, 1000)) continue;
        ok = state == DHCPACK;
    }
    udp_unbind(DHCP_CLIENT_PORT);
    if (!ok) return false;
    net_if.ip = offer_ip;
    net_if.mask = opt_mask ? opt_mask : IP4(255, 255, 255, 0);
    net_if.gw = opt_router;
    net_if.dns = opt_dns ? opt_dns : opt_router;
    net_if.configured = true;
    return true;
}
