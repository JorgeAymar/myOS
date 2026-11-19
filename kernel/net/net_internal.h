#pragma once
// Private interfaces shared by the network stack files (kernel/net/*) and the
// NIC driver (kernel/drivers/e1000.c). Not part of the public API (see net.h).
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "net.h"

static inline uint16_t htons(uint16_t v) { return __builtin_bswap16(v); }
static inline uint16_t ntohs(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }

// Unaligned big-endian accessors for packet parsing/building.
static inline uint16_t rd16(const void *p) {
    const uint8_t *b = p; return (uint16_t)((b[0] << 8) | b[1]);
}
static inline uint32_t rd32(const void *p) {
    const uint8_t *b = p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}
static inline void wr16(void *p, uint16_t v) {
    uint8_t *b = p; b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v;
}
static inline void wr32(void *p, uint32_t v) {
    uint8_t *b = p;
    b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v;
}

#define ETH_MTU       1500
#define ETH_HLEN      14
#define ETH_FRAME_MAX 1518
#define ETHERTYPE_IP  0x0800
#define ETHERTYPE_ARP 0x0806
#define IP_HLEN       20
#define IPPROTO_ICMP  1
#define IPPROTO_TCP   6
#define IPPROTO_UDP   17

struct netif {
    bool up, configured;
    uint8_t mac[6];
    uint32_t ip, mask, gw, dns;
};
extern struct netif net_if;
extern bool net_in_poll;        // true while net_poll() dispatches frames

// ---- NIC driver (e1000.c) ----
bool e1000_init(uint8_t mac_out[6], const char **model_out);
bool e1000_link_up(void);
bool e1000_send(const void *frame, size_t len);   // copies the frame
size_t e1000_recv(void *buf, size_t max);         // 0 if nothing pending

// ---- Ethernet / ARP (eth.c) ----
extern const uint8_t eth_broadcast[6];
bool eth_send(const uint8_t dst[6], uint16_t type, const void *payload, size_t len);
void eth_input(const uint8_t *frame, size_t len);
bool arp_lookup(uint32_t ip, uint8_t mac[6]);
void arp_request(uint32_t ip);
// Blocking resolve (polls). Must not be called from inside net_poll().
bool arp_resolve(uint32_t ip, uint8_t mac[6], uint32_t timeout_ms);

// ---- IPv4 / ICMP (ip.c) ----
uint32_t csum_add(uint32_t sum, const void *data, size_t len);
uint16_t csum_fold(uint32_t sum);
// Pseudo-header partial sum for TCP/UDP checksums.
uint32_t csum_pseudo(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len);
// Sends one IPv4 datagram (no fragmentation; len <= 1480). src is our IP (or
// 0.0.0.0 before DHCP). Returns 0 or a negative error.
int ip_send(uint32_t dst, uint8_t proto, const void *payload, size_t len);
void ip_input(const uint8_t *pkt, size_t len);
void icmp_input(uint32_t src, const uint8_t *data, size_t len);

// ---- UDP / TCP (udp.c, tcp.c) ----
void udp_input(uint32_t src, uint32_t dst, const uint8_t *data, size_t len);
void tcp_input(uint32_t src, uint32_t dst, const uint8_t *data, size_t len);
void tcp_timers(uint64_t now);

// ---- DHCP (dhcp.c) ----
bool dhcp_run(uint32_t timeout_ms);

// Waits for the next interrupt (hlt) if interrupts are enabled, else spins briefly.
void net_idle(void);

// Busy-wait helper: polls the network until cond is true or timeout. Returns cond.
#define NET_WAIT(cond, timeout_ms) ({                                   \
    uint64_t _dl = timer_ticks_ms() + (timeout_ms);                     \
    while (!(cond) && timer_ticks_ms() < _dl) { net_poll(); net_idle(); } \
    (bool)(cond);                                                       \
})
