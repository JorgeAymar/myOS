// Ethernet II framing and ARP (cache, request/reply). Owner: net agent.
#include "net_internal.h"
#include "arch.h"
#include "string.h"

const uint8_t eth_broadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

#define ARP_ENTRIES 16
#define ARP_TTL_MS  (10 * 60 * 1000)

struct arp_entry {
    uint32_t ip;
    uint8_t mac[6];
    uint64_t stamp;
    bool valid;
};
static struct arp_entry arp_cache[ARP_ENTRIES];

bool eth_send(const uint8_t dst[6], uint16_t type, const void *payload, size_t len) {
    uint8_t frame[ETH_FRAME_MAX];
    if (len > ETH_MTU) return false;
    memcpy(frame, dst, 6);
    memcpy(frame + 6, net_if.mac, 6);
    wr16(frame + 12, type);
    memcpy(frame + ETH_HLEN, payload, len);
    return e1000_send(frame, ETH_HLEN + len);
}

static void arp_store(uint32_t ip, const uint8_t mac[6]) {
    if (!ip) return;
    struct arp_entry *slot = 0, *oldest = &arp_cache[0];
    for (int i = 0; i < ARP_ENTRIES; i++) {
        struct arp_entry *e = &arp_cache[i];
        if (e->valid && e->ip == ip) { slot = e; break; }
        if (!e->valid && !slot) slot = e;
        if (e->stamp < oldest->stamp) oldest = e;
    }
    if (!slot) slot = oldest;
    slot->ip = ip;
    memcpy(slot->mac, mac, 6);
    slot->stamp = timer_ticks_ms();
    slot->valid = true;
}

bool arp_lookup(uint32_t ip, uint8_t mac[6]) {
    uint64_t now = timer_ticks_ms();
    for (int i = 0; i < ARP_ENTRIES; i++) {
        struct arp_entry *e = &arp_cache[i];
        if (e->valid && e->ip == ip) {
            if (now - e->stamp > ARP_TTL_MS) { e->valid = false; return false; }
            memcpy(mac, e->mac, 6);
            return true;
        }
    }
    return false;
}

static void arp_send(uint16_t op, const uint8_t tha[6], uint32_t tpa, const uint8_t dst[6]) {
    uint8_t p[28];
    wr16(p, 1);              // hardware: Ethernet
    wr16(p + 2, ETHERTYPE_IP);
    p[4] = 6; p[5] = 4;
    wr16(p + 6, op);
    memcpy(p + 8, net_if.mac, 6);
    wr32(p + 14, net_if.ip);
    memcpy(p + 18, tha, 6);
    wr32(p + 24, tpa);
    eth_send(dst, ETHERTYPE_ARP, p, sizeof p);
}

void arp_request(uint32_t ip) {
    static const uint8_t zero[6];
    arp_send(1, zero, ip, eth_broadcast);
}

bool arp_resolve(uint32_t ip, uint8_t mac[6], uint32_t timeout_ms) {
    if (arp_lookup(ip, mac)) return true;
    uint64_t deadline = timer_ticks_ms() + timeout_ms;
    while (timer_ticks_ms() < deadline) {
        arp_request(ip);
        uint32_t left = (uint32_t)(deadline - timer_ticks_ms());
        if (NET_WAIT(arp_lookup(ip, mac), left < 250 ? left : 250)) return true;
    }
    return arp_lookup(ip, mac);
}

static void arp_input(const uint8_t *p, size_t len) {
    if (len < 28 || rd16(p) != 1 || rd16(p + 2) != ETHERTYPE_IP || p[4] != 6 || p[5] != 4) return;
    uint16_t op = rd16(p + 6);
    uint32_t spa = rd32(p + 14), tpa = rd32(p + 24);
    const uint8_t *sha = p + 8;
    bool for_us = net_if.ip && tpa == net_if.ip;
    // Learn/refresh the sender if it targets us or is already cached.
    uint8_t tmp[6];
    if (for_us || op == 2 || arp_lookup(spa, tmp)) arp_store(spa, sha);
    if (op == 1 && for_us) arp_send(2, sha, spa, sha);
}

void eth_input(const uint8_t *frame, size_t len) {
    if (len < ETH_HLEN) return;
    if (memcmp(frame, net_if.mac, 6) != 0 && memcmp(frame, eth_broadcast, 6) != 0) return;
    uint16_t type = rd16(frame + 12);
    if (type == ETHERTYPE_ARP) arp_input(frame + ETH_HLEN, len - ETH_HLEN);
    else if (type == ETHERTYPE_IP) ip_input(frame + ETH_HLEN, len - ETH_HLEN);
}
