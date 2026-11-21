// HTTPS client: BearSSL client engine (TLS 1.2) over the kernel TCP client.
// Certificates are validated against the Mozilla roots in ca_roots.c.
#include "net_internal.h"
#include "tls.h"
#include "ca_roots.h"
#include "arch.h"
#include "mm.h"
#include "kprintf.h"
#include "string.h"
#include "proc.h"
#include <bearssl.h>

struct tls_ctx {
    br_ssl_client_context cc;
    br_x509_minimal_context xc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
};

int https_get_timeout(const char *host, const char *path, char *buf, size_t len,
                      uint32_t timeout_ms) {
    if (!buf || len == 0 || !host) return NET_ERR;
    buf[0] = 0;
    // Phase timing, printed once as "[tls] timing dns=..ms tcp=..ms hs=..ms ttfb=..ms body=..KB/..ms":
    //   dns   = name resolution
    //   tcp   = TCP three-way handshake to port 443
    //   hs    = from TCP established until the TLS handshake is done (first app byte can be sent)
    //   ttfb  = from request sent until the first response byte is decrypted
    //   body  = application bytes received (KB) and ms from the first byte to the end
    uint64_t t_start = timer_ticks_ms(), t_dns = 0, t_tcp = 0, t_hs = 0, t_req = 0, t_first = 0;
    uint64_t deadline = t_start + timeout_ms;
    uint32_t ip;
    int r = dns_resolve(host, &ip, timeout_ms);
    if (r < 0) return r;
    uint64_t now = timer_ticks_ms();
    t_dns = now;
    if (now >= deadline) return NET_ETIMEDOUT;

    struct tls_ctx *t = kmalloc(sizeof *t);
    if (!t) return NET_ERR;
    struct tcp_conn *c = tcp_connect(ip, 443, (uint32_t)(deadline - now));
    if (!c) { kfree(t); return NET_ETIMEDOUT; }
    t_tcp = timer_ticks_ms();

    char req[512];
    int rn = ksnprintf(req, sizeof req,
                       "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: myOS/0.1\r\n"
                       "Accept: */*\r\nConnection: close\r\n\r\n",
                       (path && *path) ? path : "/", host);
    if (rn <= 0 || (size_t)rn >= sizeof req) { tcp_close(c); kfree(t); return NET_ERR; }

    br_ssl_client_init_full(&t->cc, &t->xc, tls_TAs, tls_TAs_NUM);
    uint64_t ut = tls_unix_time();
    br_x509_minimal_set_time(&t->xc, (uint32_t)(ut / 86400 + 719528), (uint32_t)(ut % 86400));
    br_ssl_engine_set_buffer(&t->cc.eng, t->iobuf, sizeof t->iobuf, 1);
    uint8_t seed[32];
    tls_entropy(seed, sizeof seed);
    br_ssl_engine_inject_entropy(&t->cc.eng, seed, sizeof seed);
    if (!br_ssl_client_reset(&t->cc, host, 0)) {
        kprintf("[tls] error %d\n", br_ssl_engine_last_error(&t->cc.eng));
        tcp_close(c); kfree(t); return NET_ERR;
    }

    br_ssl_engine_context *eng = &t->cc.eng;
    size_t got = 0, reqoff = 0;
    int result = 0;        // negative NET_E* once decided
    bool full = false;
    for (;;) {
        unsigned st = br_ssl_engine_current_state(eng);
        if (st & BR_SSL_CLOSED) break;
        now = timer_ticks_ms();
        if (now >= deadline) { result = got ? 0 : NET_ETIMEDOUT; break; }
        uint32_t left = (uint32_t)(deadline - now);
        size_t n;
        unsigned char *b;
        if (st & BR_SSL_SENDREC) {
            b = br_ssl_engine_sendrec_buf(eng, &n);
            r = tcp_send(c, b, n);
            if (r < 0) { result = got ? 0 : r; break; }
            br_ssl_engine_sendrec_ack(eng, n);
        } else if (st & BR_SSL_RECVAPP) {
            b = br_ssl_engine_recvapp_buf(eng, &n);
            size_t room = len - 1 - got;
            size_t k = n < room ? n : room;
            memcpy(buf + got, b, k);
            if (!got && k) t_first = timer_ticks_ms();
            got += k;
            br_ssl_engine_recvapp_ack(eng, n);
            if (got + 1 >= len) { full = true; break; }   // truncate like http.c
        } else if ((st & BR_SSL_SENDAPP) && reqoff < (size_t)rn) {
            if (!t_hs) t_hs = timer_ticks_ms();
            b = br_ssl_engine_sendapp_buf(eng, &n);
            size_t k = (size_t)rn - reqoff;
            if (k > n) k = n;
            memcpy(b, req + reqoff, k);
            reqoff += k;
            br_ssl_engine_sendapp_ack(eng, k);
            if (reqoff == (size_t)rn) { br_ssl_engine_flush(eng, 0); t_req = timer_ticks_ms(); }
        } else if (st & BR_SSL_RECVREC) {
            b = br_ssl_engine_recvrec_buf(eng, &n);
            r = tcp_recv(c, b, n, left);
            if (r > 0) br_ssl_engine_recvrec_ack(eng, (size_t)r);
            else if (r == 0) { br_ssl_engine_close(eng); break; }   // FIN (Connection: close) without close_notify: done.
            // Do NOT keep looping: the closed engine still reports RECVREC and would spin to the deadline.
            else { result = got ? 0 : r; break; }
        } else {
            // SENDAPP with the request fully sent: nothing to do, wait for data.
            br_ssl_engine_flush(eng, 0);
            thread_sleep_ms(1);
        }
    }
    if (!full && result == 0) {
        int err = br_ssl_engine_last_error(eng);
        if (err != BR_ERR_OK && !(err == BR_ERR_IO && got)) {
            kprintf("[tls] error %d\n", err);
            result = NET_ERR;
        } else if (!got) result = NET_ECLOSED;
    }
    buf[got] = 0;
    {
        uint64_t te = timer_ticks_ms();
        if (!t_hs) t_hs = te;
        if (!t_req) t_req = t_hs;
        if (!t_first) t_first = te;
        kprintf("[tls] timing dns=%lums tcp=%lums hs=%lums ttfb=%lums body=%luKB/%lums\n",
                (unsigned long)(t_dns - t_start), (unsigned long)(t_tcp - t_dns),
                (unsigned long)(t_hs - t_tcp), (unsigned long)(t_first - t_req),
                (unsigned long)(got / 1024), (unsigned long)(te - t_first));
    }
    tcp_close(c);
    kfree(t);
    if (result < 0 && !got) return result;
    return (int)got;
}

// ---- boot self-check (test mode) -------------------------------------------

struct tls_st { const char *host; int r; char *buf; size_t len; uint32_t ms; };

static void tls_st_thread(void *arg) {
    struct tls_st *s = arg;
    // Paint the stack below us and measure how much BearSSL uses (thread stacks are 16 KB).
    volatile uint8_t *sp = (volatile uint8_t *)__builtin_frame_address(0);
    enum { PAINT = 11 * 1024 };
    volatile uint8_t *lo = sp - PAINT - 512;
    for (volatile uint8_t *p = lo; p < sp - 512; p++) *p = 0xA5;
    s->r = https_get_timeout(s->host, "/", s->buf, s->len, s->ms);
    size_t untouched = 0;
    for (volatile uint8_t *p = lo; p < sp - 512 && *p == 0xA5; p++) untouched++;
    kprintf("[tls] stack high-water about %lu bytes below entry (painted %d)\n",
            (unsigned long)(PAINT - untouched + 512), PAINT);
}

static int tls_run(const char *host, char *buf, size_t len, uint32_t ms) {
    struct tls_st s = { host, NET_ERR, buf, len, ms };
    tid_t tid = thread_create("tls-selftest", tls_st_thread, &s);
    if (tid < 0) return NET_ERR;
    thread_join(tid);
    return s.r;
}

void tls_https_selftest(void) {
    static char resp[2048];
    int r = tls_run("example.com", resp, sizeof resp, 8000);
    if (r > 0 && strncmp(resp, "HTTP/", 5) == 0) kprintf("[OK] tls https example.com (%d bytes)\n", r);
    else kprintf("[WARN] tls https selftest failed (%d)\n", r);

    r = tls_run("self-signed.badssl.com", resp, sizeof resp, 8000);
    if (r == NET_ERR) kprintf("[OK] tls rejects bad cert\n");
    else if (r < 0) kprintf("[WARN] tls bad-cert test inconclusive (%d)\n", r);
    else kprintf("[FAIL] tls accepted a self-signed certificate (%d bytes)\n", r);
}

// BearSSL sysrng.c is excluded (no OS RNG); the engine is always seeded explicitly
// via br_ssl_engine_inject_entropy(), so report "no system seeder".
br_prng_seeder br_prng_seeder_system(const char **name) {
    if (name) *name = NULL;
    return 0;
}
