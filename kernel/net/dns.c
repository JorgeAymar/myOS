// DNS stub resolver (A records over UDP, small positive cache). Owner: net agent.
#include "net_internal.h"
#include "arch.h"
#include "string.h"

#define DNS_PORT 53
#define DNS_CACHE 8

static uint16_t query_id, query_port;
static int result;               // 0 pending, 1 found, NET_ENOTFOUND
static uint32_t result_ip;

static struct { char name[64]; uint32_t ip; bool valid; } cache[DNS_CACHE];
static int cache_next;

// Skips an encoded (possibly compressed) name. Returns new offset or 0 on error.
static size_t skip_name(const uint8_t *p, size_t len, size_t off) {
    while (off < len) {
        uint8_t l = p[off];
        if (l == 0) return off + 1;
        if ((l & 0xc0) == 0xc0) return off + 2 <= len ? off + 2 : 0;
        if (l & 0xc0) return 0;
        off += 1 + (size_t)l;
    }
    return 0;
}

static void dns_handler(uint32_t src, uint16_t sport, const void *data, size_t len) {
    (void)src;
    const uint8_t *p = data;
    if (sport != DNS_PORT || len < 12 || rd16(p) != query_id || !(p[2] & 0x80) || result) return;
    uint16_t flags = rd16(p + 2);
    uint16_t qd = rd16(p + 4), an = rd16(p + 6);
    if ((flags & 0xf) != 0) { result = NET_ENOTFOUND; return; }   // NXDOMAIN etc.
    size_t off = 12;
    for (uint16_t i = 0; i < qd; i++) {
        off = skip_name(p, len, off);
        if (!off || off + 4 > len) { result = NET_ENOTFOUND; return; }
        off += 4;
    }
    for (uint16_t i = 0; i < an; i++) {
        off = skip_name(p, len, off);
        if (!off || off + 10 > len) break;
        uint16_t type = rd16(p + off), cls = rd16(p + off + 2), rdlen = rd16(p + off + 8);
        off += 10;
        if (off + rdlen > len) break;
        if (type == 1 && cls == 1 && rdlen == 4) {
            result_ip = rd32(p + off);
            result = 1;
            return;
        }
        off += rdlen;   // CNAME and others: keep scanning
    }
    result = NET_ENOTFOUND;
}

static size_t build_query(uint8_t *q, const char *name) {
    memset(q, 0, 12);
    wr16(q, query_id);
    wr16(q + 2, 0x0100);         // standard query, recursion desired
    wr16(q + 4, 1);
    size_t off = 12;
    const char *s = name;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t l = dot ? (size_t)(dot - s) : strlen(s);
        if (l == 0 || l > 63 || off + l + 2 > 270) return 0;
        q[off++] = (uint8_t)l;
        memcpy(q + off, s, l);
        off += l;
        s += l;
        if (*s == '.') s++;
    }
    q[off++] = 0;
    wr16(q + off, 1); wr16(q + off + 2, 1);   // QTYPE A, QCLASS IN
    return off + 4;
}

int dns_resolve(const char *name, uint32_t *ip, uint32_t timeout_ms) {
    if (ip_parse(name, ip)) return 0;
    if (!net_if.configured || !net_if.dns) return NET_ERR;
    size_t nlen = strlen(name);
    if (nlen == 0 || nlen > 253) return NET_ENOTFOUND;
    for (int i = 0; i < DNS_CACHE; i++)
        if (cache[i].valid && strcmp(cache[i].name, name) == 0) { *ip = cache[i].ip; return 0; }

    query_port = net_ephemeral_port();
    if (!udp_bind(query_port, dns_handler)) return NET_ERR;
    uint64_t deadline = timer_ticks_ms() + timeout_ms;
    int r = NET_ETIMEDOUT;
    uint8_t q[280];
    while (timer_ticks_ms() < deadline) {
        query_id = (uint16_t)(timer_ticks_ms() * 31 + query_id + 1);
        size_t qlen = build_query(q, name);
        if (!qlen) { r = NET_ENOTFOUND; break; }
        result = 0;
        if (udp_send(net_if.dns, query_port, DNS_PORT, q, qlen) < 0) { r = NET_ERR; break; }
        uint64_t now = timer_ticks_ms();
        uint64_t wait = deadline > now ? deadline - now : 0;
        if (wait > 1000) wait = 1000;              // retransmit every second
        if (NET_WAIT(result != 0, wait)) {
            r = result == 1 ? 0 : result;
            break;
        }
    }
    udp_unbind(query_port);
    if (r == 0) {
        *ip = result_ip;
        if (nlen < sizeof cache[0].name) {
            strcpy(cache[cache_next].name, name);
            cache[cache_next].ip = result_ip;
            cache[cache_next].valid = true;
            cache_next = (cache_next + 1) % DNS_CACHE;
        }
    }
    return r;
}
