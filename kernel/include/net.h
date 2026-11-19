#pragma once
// Networking: Intel e1000 NIC driver + a small TCP/IP stack (Ethernet, ARP,
// IPv4, ICMP, UDP, DHCP, DNS, TCP, HTTP/1.0). Owner: net agent (kernel/net/,
// kernel/drivers/e1000.c).
//
// Concurrency model: everything runs on the caller's thread. Received frames are
// only processed inside net_poll(); every blocking call below loops on
// net_poll() + timer_ticks_ms() until done or its timeout expires, so it works
// with or without a scheduler. The stack is not reentrant: call it from a
// single thread (or serialise callers with a lock).
//
// IPv4 addresses are uint32_t in HOST byte order: 10.0.2.15 == IP4(10,0,2,15).
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define IP4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                         ((uint32_t)(c) << 8) | (uint32_t)(d))

// Error codes returned (negated) by the calls below.
#define NET_ERR         (-1)   // generic failure / no interface
#define NET_ETIMEDOUT   (-2)
#define NET_ERESET      (-3)   // connection reset by peer
#define NET_ENOTFOUND   (-4)   // DNS name does not exist / no A record
#define NET_ECLOSED     (-5)   // connection not open

struct net_ifconfig {
    bool up;            // NIC present and link up
    bool configured;    // DHCP succeeded
    uint8_t mac[6];
    uint32_t ip, mask, gw, dns;
};

// Subsystem init (called by kmain after fs_init): NIC bring-up, DHCP and boot
// self-tests (ARP gateway, ping, DNS, HTTP). Prints "[OK] net ..." lines.
void net_init(void);
// Processes all pending received frames and runs TCP timers. Call often (main
// loop / idle). Cheap when nothing arrived.
void net_poll(void);
// Copies the current interface configuration. Returns false if no NIC.
bool net_ifconfig(struct net_ifconfig *out);

// Formats ip as "a.b.c.d" into buf (>= 16 bytes). Returns buf.
char *ip_to_str(uint32_t ip, char *buf);
// Parses "a.b.c.d". Returns false if s is not a dotted quad.
bool ip_parse(const char *s, uint32_t *ip);

// ICMP echo. Returns the round-trip time in ms (>= 0) or a negative error.
int net_ping(uint32_t ip, uint32_t timeout_ms);

// UDP. Handlers run inside net_poll(). Ports are host byte order.
typedef void (*udp_handler_t)(uint32_t src_ip, uint16_t src_port,
                              const void *data, size_t len);
bool udp_bind(uint16_t port, udp_handler_t handler);   // false if port busy / table full
void udp_unbind(uint16_t port);
int udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
             const void *data, size_t len);            // 0 or negative error
uint16_t net_ephemeral_port(void);

// DNS A-record lookup via the DHCP-provided server. Dotted quads are parsed
// directly. Returns 0 and sets *ip, or a negative error.
int dns_resolve(const char *name, uint32_t *ip, uint32_t timeout_ms);

// TCP client. A connection handle is an opaque pointer.
struct tcp_conn;
// Active open (3-way handshake). Returns NULL on failure (timeout/RST).
struct tcp_conn *tcp_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms);
// Queues len bytes (blocks while the send buffer is full). Returns bytes queued
// (== len on success) or a negative error.
int tcp_send(struct tcp_conn *c, const void *data, size_t len);
// Receives up to len bytes, waiting up to timeout_ms for data. Returns the byte
// count (> 0), 0 at end of stream (peer sent FIN), or a negative error.
int tcp_recv(struct tcp_conn *c, void *buf, size_t len, uint32_t timeout_ms);
// Sends FIN, waits briefly for the close handshake and frees the connection.
void tcp_close(struct tcp_conn *c);

// HTTP/1.0 GET of http://host/path (port 80). Stores the raw response (status
// line + headers + body), NUL-terminated and truncated to len-1 bytes. Returns
// the number of bytes stored or a negative error.
int http_get(const char *host, const char *path, char *buf, size_t len);
int http_get_timeout(const char *host, const char *path, char *buf, size_t len,
                     uint32_t timeout_ms);
