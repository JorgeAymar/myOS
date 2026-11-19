// Minimal HTTP/1.0 GET client over the TCP client. Owner: net agent.
#include "net_internal.h"
#include "arch.h"
#include "kprintf.h"
#include "string.h"

int http_get_timeout(const char *host, const char *path, char *buf, size_t len,
                     uint32_t timeout_ms) {
    if (!buf || len == 0) return NET_ERR;
    buf[0] = 0;
    uint64_t deadline = timer_ticks_ms() + timeout_ms;
    uint32_t ip;
    int r = dns_resolve(host, &ip, timeout_ms);
    if (r < 0) return r;
    uint64_t now = timer_ticks_ms();
    if (now >= deadline) return NET_ETIMEDOUT;
    struct tcp_conn *c = tcp_connect(ip, 80, (uint32_t)(deadline - now));
    if (!c) return NET_ETIMEDOUT;

    char req[512];
    int n = ksnprintf(req, sizeof req,
                      "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: myOS/0.1\r\n"
                      "Accept: */*\r\nConnection: close\r\n\r\n",
                      (path && *path) ? path : "/", host);
    if (n <= 0 || (size_t)n >= sizeof req) { tcp_close(c); return NET_ERR; }
    r = tcp_send(c, req, (size_t)n);
    if (r < 0) { tcp_close(c); return r; }

    size_t got = 0;
    for (;;) {
        now = timer_ticks_ms();
        if (now >= deadline) { r = got ? 0 : NET_ETIMEDOUT; break; }
        if (got + 1 >= len) { r = 0; break; }        // buffer full: truncate
        r = tcp_recv(c, buf + got, len - 1 - got, (uint32_t)(deadline - now));
        if (r <= 0) break;                           // 0 = server closed (normal end)
        got += (size_t)r;
    }
    buf[got] = 0;
    tcp_close(c);
    if (r < 0 && !got) return r;
    return (int)got;
}

int http_get(const char *host, const char *path, char *buf, size_t len) {
    return http_get_timeout(host, path, buf, len, 10000);
}
