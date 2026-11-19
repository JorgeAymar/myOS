// TCP client: active open (3-way handshake), sequence/ack tracking, go-back-N
// retransmission with exponential backoff, in-order receive into a ring buffer
// (out-of-order segments are dropped and re-ACKed), peer window respected,
// FIN close in both directions, RST handling. Owner: net agent.
#include "net_internal.h"
#include "arch.h"
#include "mm.h"
#include "string.h"

#define TCP_HLEN    20
#define TCP_MAXCONN 8
#define TCP_RXBUF   16384
#define TCP_TXBUF   8192
#define TCP_MSS     (ETH_MTU - IP_HLEN - TCP_HLEN)
#define RTO_INIT    500
#define RTO_MAX     4000
#define MAX_RETRIES 7

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

enum { CLOSED, SYN_SENT, ESTABLISHED, FIN_WAIT_1, FIN_WAIT_2, CLOSING, TIME_WAIT,
       CLOSE_WAIT, LAST_ACK };

struct tcp_conn {
    int state;
    int error;                     // sticky error (NET_ERESET / NET_ETIMEDOUT)
    uint32_t rip;
    uint16_t lport, rport;
    // send side
    uint32_t iss, snd_una, snd_nxt, snd_wnd;
    uint16_t peer_mss;
    uint8_t tx[TCP_TXBUF];         // bytes starting at snd_una
    size_t tx_count, tx_sent;      // queued / of which in flight
    bool fin_queued, fin_sent, fin_acked;
    uint64_t rto_deadline;         // 0 = timer stopped
    uint32_t rto;
    int retries;
    // receive side
    uint32_t rcv_nxt;
    uint8_t rx[TCP_RXBUF];
    size_t rx_head, rx_count;
    uint32_t adv_wnd;              // last advertised window
    bool fin_rcvd;
};

static struct tcp_conn *conns[TCP_MAXCONN];
static uint32_t iss_counter;

static inline int32_t seq_diff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

static uint32_t rx_free(const struct tcp_conn *c) { return (uint32_t)(TCP_RXBUF - c->rx_count); }

static void send_raw(uint32_t dst, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
                     uint8_t flags, uint16_t wnd, const uint8_t *data, size_t len, bool mss_opt) {
    uint8_t seg[ETH_MTU - IP_HLEN];
    size_t hlen = TCP_HLEN + (mss_opt ? 4 : 0);
    if (hlen + len > sizeof seg) return;
    wr16(seg, sport);
    wr16(seg + 2, dport);
    wr32(seg + 4, seq);
    wr32(seg + 8, ack);
    seg[12] = (uint8_t)((hlen / 4) << 4);
    seg[13] = flags;
    wr16(seg + 14, wnd);
    wr16(seg + 16, 0);
    wr16(seg + 18, 0);
    if (mss_opt) { seg[20] = 2; seg[21] = 4; wr16(seg + 22, TCP_MSS); }
    if (len) memcpy(seg + hlen, data, len);
    uint16_t tl = (uint16_t)(hlen + len);
    wr16(seg + 16, csum_fold(csum_add(csum_pseudo(net_if.ip, dst, IPPROTO_TCP, tl), seg, tl)));
    ip_send(dst, IPPROTO_TCP, seg, tl);
}

static void send_seg(struct tcp_conn *c, uint8_t flags, uint32_t seq, const uint8_t *data, size_t len) {
    uint32_t wnd = rx_free(c);
    if (wnd > 0xffff) wnd = 0xffff;
    c->adv_wnd = wnd;
    if (c->state != SYN_SENT) flags |= F_ACK;
    send_raw(c->rip, c->lport, c->rport, seq, c->rcv_nxt, flags, (uint16_t)wnd, data, len,
             (flags & F_SYN) != 0);
}

static void send_ack(struct tcp_conn *c) { send_seg(c, F_ACK, c->snd_nxt, 0, 0); }

static void timer_start(struct tcp_conn *c) {
    if (!c->rto_deadline) c->rto_deadline = timer_ticks_ms() + c->rto;
}

// Sends queued data the peer's window allows, then FIN if requested.
// force_probe: send at least one byte even into a zero window (persist probe).
static void tcp_output(struct tcp_conn *c, bool force_probe) {
    if (c->state != ESTABLISHED && c->state != CLOSE_WAIT && c->state != FIN_WAIT_1 &&
        c->state != CLOSING && c->state != LAST_ACK) return;
    uint32_t mss = c->peer_mss < TCP_MSS ? c->peer_mss : TCP_MSS;
    while (c->tx_sent < c->tx_count) {
        uint32_t in_flight = (uint32_t)c->tx_sent;
        uint32_t usable = c->snd_wnd > in_flight ? c->snd_wnd - in_flight : 0;
        if (!usable && force_probe && in_flight == 0) usable = 1;
        if (!usable) break;
        size_t n = c->tx_count - c->tx_sent;
        if (n > mss) n = mss;
        if (n > usable) n = usable;
        send_seg(c, F_PSH, c->snd_una + (uint32_t)c->tx_sent, c->tx + c->tx_sent, n);
        c->tx_sent += n;
        c->snd_nxt = c->snd_una + (uint32_t)c->tx_sent;
        timer_start(c);
    }
    if (c->tx_count && c->tx_sent == 0 && !c->snd_wnd) timer_start(c);   // zero-window persist
    if (c->fin_queued && !c->fin_sent && !c->fin_acked && c->tx_sent == c->tx_count) {
        send_seg(c, F_FIN, c->snd_nxt, 0, 0);
        c->snd_nxt++;
        c->fin_sent = true;
        if (c->state == ESTABLISHED) c->state = FIN_WAIT_1;
        else if (c->state == CLOSE_WAIT) c->state = LAST_ACK;
        timer_start(c);
    }
}

static void conn_fail(struct tcp_conn *c, int err) {
    c->error = err;
    c->state = CLOSED;
    c->rto_deadline = 0;
}

void tcp_timers(uint64_t now) {
    for (int i = 0; i < TCP_MAXCONN; i++) {
        struct tcp_conn *c = conns[i];
        if (!c || c->state == CLOSED || !c->rto_deadline || now < c->rto_deadline) continue;
        c->rto_deadline = 0;
        if (++c->retries > MAX_RETRIES) { conn_fail(c, NET_ETIMEDOUT); continue; }
        c->rto = c->rto * 2 > RTO_MAX ? RTO_MAX : c->rto * 2;
        if (c->state == SYN_SENT) {
            send_seg(c, F_SYN, c->iss, 0, 0);
            timer_start(c);
            continue;
        }
        // Go-back-N: resend everything unacknowledged.
        c->tx_sent = 0;
        c->snd_nxt = c->snd_una;
        if (c->fin_sent && !c->fin_acked) c->fin_sent = false;
        tcp_output(c, true);
    }
}

static struct tcp_conn *find_conn(uint32_t src, uint16_t sport, uint16_t dport) {
    for (int i = 0; i < TCP_MAXCONN; i++) {
        struct tcp_conn *c = conns[i];
        if (c && c->state != CLOSED && c->rip == src && c->rport == sport && c->lport == dport)
            return c;
    }
    return 0;
}

static void rx_store(struct tcp_conn *c, const uint8_t *data, size_t n) {
    for (size_t i = 0; i < n; i++)
        c->rx[(c->rx_head + c->rx_count + i) % TCP_RXBUF] = data[i];
    c->rx_count += n;
}

void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len) {
    if (len < TCP_HLEN) return;
    if (csum_fold(csum_add(csum_pseudo(src, dst, IPPROTO_TCP, (uint16_t)len), seg, len)) != 0) return;
    uint16_t sport = rd16(seg), dport = rd16(seg + 2);
    uint32_t seq = rd32(seg + 4), ack = rd32(seg + 8);
    size_t hlen = (size_t)(seg[12] >> 4) * 4;
    uint8_t flags = seg[13];
    uint32_t wnd = rd16(seg + 14);
    if (hlen < TCP_HLEN || hlen > len) return;
    const uint8_t *data = seg + hlen;
    size_t plen = len - hlen;

    struct tcp_conn *c = find_conn(src, sport, dport);
    if (!c) {
        if (flags & F_RST) return;
        if (flags & F_ACK) send_raw(src, dport, sport, ack, 0, F_RST, 0, 0, 0, false);
        else send_raw(src, dport, sport, 0, seq + (uint32_t)plen + ((flags & F_SYN) ? 1 : 0) +
                      ((flags & F_FIN) ? 1 : 0), F_RST | F_ACK, 0, 0, 0, false);
        return;
    }

    if (c->state == SYN_SENT) {
        bool ack_ok = (flags & F_ACK) && ack == c->iss + 1;
        if ((flags & F_ACK) && !ack_ok) {
            if (!(flags & F_RST)) send_raw(src, dport, sport, ack, 0, F_RST, 0, 0, 0, false);
            return;
        }
        if (flags & F_RST) { if (ack_ok) conn_fail(c, NET_ERESET); return; }
        if ((flags & F_SYN) && ack_ok) {
            c->rcv_nxt = seq + 1;
            c->peer_mss = 536;
            for (size_t o = TCP_HLEN; o < hlen;) {          // parse options for MSS
                uint8_t kind = seg[o];
                if (kind == 0) break;
                if (kind == 1) { o++; continue; }
                if (o + 1 >= hlen || seg[o + 1] < 2) break;
                if (kind == 2 && seg[o + 1] == 4 && o + 4 <= hlen) c->peer_mss = rd16(seg + o + 2);
                o += seg[o + 1];
            }
            c->snd_una = ack;
            c->snd_nxt = ack;
            c->snd_wnd = wnd;
            c->state = ESTABLISHED;
            c->rto_deadline = 0;
            c->retries = 0;
            c->rto = RTO_INIT;
            send_ack(c);
        }
        return;
    }

    if (flags & F_RST) {
        uint32_t off = seq - c->rcv_nxt;
        if (off <= TCP_RXBUF || c->state == TIME_WAIT) conn_fail(c, NET_ERESET);
        return;
    }
    if (flags & F_SYN) { send_ack(c); return; }        // retransmitted SYN-ACK
    if (!(flags & F_ACK)) return;

    // ---- ACK processing ----
    int32_t acked = seq_diff(ack, c->snd_una);
    if (acked > 0 && seq_diff(ack, c->snd_nxt) <= 0) {
        size_t data_acked = (size_t)acked < c->tx_sent ? (size_t)acked : c->tx_sent;
        memmove(c->tx, c->tx + data_acked, c->tx_count - data_acked);
        c->tx_count -= data_acked;
        c->tx_sent -= data_acked;
        c->snd_una = ack;
        if ((size_t)acked > data_acked && c->fin_sent) c->fin_acked = true;
        c->retries = 0;
        c->rto = RTO_INIT;
        c->rto_deadline = 0;
        if (c->snd_una != c->snd_nxt) timer_start(c);
    }
    if (seq_diff(ack, c->snd_una) >= 0 && seq_diff(ack, c->snd_nxt) <= 0) c->snd_wnd = wnd;
    if (c->fin_acked) {
        if (c->state == FIN_WAIT_1) c->state = FIN_WAIT_2;
        else if (c->state == CLOSING) c->state = TIME_WAIT;
        else if (c->state == LAST_ACK) { c->state = CLOSED; c->rto_deadline = 0; return; }
    }

    // ---- data / FIN ----
    bool fin = (flags & F_FIN) != 0;
    bool need_ack = false;
    if (c->state == ESTABLISHED || c->state == FIN_WAIT_1 || c->state == FIN_WAIT_2) {
        int32_t d = seq_diff(c->rcv_nxt, seq);
        if (plen || fin) need_ack = true;
        if (d > 0) {                         // (partly) duplicate: trim the old part
            if ((size_t)d >= plen + (fin ? 1 : 0)) { plen = 0; fin = false; }
            else { data += d; plen -= (size_t)d; seq += (uint32_t)d; }
        }
        if (seq == c->rcv_nxt) {
            size_t n = plen < rx_free(c) ? plen : rx_free(c);
            rx_store(c, data, n);
            c->rcv_nxt += (uint32_t)n;
            if (fin && n == plen) {
                c->rcv_nxt++;
                c->fin_rcvd = true;
                if (c->state == ESTABLISHED) c->state = CLOSE_WAIT;
                else if (c->state == FIN_WAIT_1) c->state = c->fin_acked ? TIME_WAIT : CLOSING;
                else c->state = TIME_WAIT;
            }
        }
    } else if (fin || plen) {
        need_ack = true;                     // e.g. retransmitted FIN in TIME_WAIT
    }
    if (need_ack) send_ack(c);
    tcp_output(c, false);                    // window may have opened
}

struct tcp_conn *tcp_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms) {
    if (!net_if.configured) return 0;
    int slot = -1;
    for (int i = 0; i < TCP_MAXCONN; i++) if (!conns[i]) { slot = i; break; }
    if (slot < 0) return 0;
    struct tcp_conn *c = kcalloc(1, sizeof *c);
    if (!c) return 0;
    c->rip = ip;
    c->rport = port;
    c->lport = net_ephemeral_port();
    c->iss = (uint32_t)timer_ticks_ms() * 64000u + (iss_counter += 0x01000193u);
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1;
    c->rto = RTO_INIT;
    c->peer_mss = 536;
    c->state = SYN_SENT;
    conns[slot] = c;

    // Resolve the next hop first (blocking) so the SYN isn't lost to ARP.
    uint8_t mac[6];
    uint32_t hop = (ip & net_if.mask) == (net_if.ip & net_if.mask) ? ip : net_if.gw;
    if (!arp_resolve(hop, mac, timeout_ms < 1000 ? timeout_ms : 1000)) {
        conns[slot] = 0; kfree(c); return 0;
    }
    send_seg(c, F_SYN, c->iss, 0, 0);
    timer_start(c);
    NET_WAIT(c->state != SYN_SENT, timeout_ms);
    if (c->state != ESTABLISHED) {
        if (c->state == SYN_SENT) send_raw(ip, c->lport, port, c->snd_nxt, 0, F_RST, 0, 0, 0, false);
        conns[slot] = 0;
        kfree(c);
        return 0;
    }
    return c;
}

int tcp_send(struct tcp_conn *c, const void *data, size_t len) {
    if (!c) return NET_ERR;
    const uint8_t *p = data;
    size_t done = 0;
    while (done < len) {
        if (c->error) return c->error;
        if ((c->state != ESTABLISHED && c->state != CLOSE_WAIT) || c->fin_queued) return NET_ECLOSED;
        size_t n = len - done;
        if (n > TCP_TXBUF - c->tx_count) n = TCP_TXBUF - c->tx_count;
        memcpy(c->tx + c->tx_count, p + done, n);
        c->tx_count += n;
        done += n;
        tcp_output(c, false);
        // Blocks while the buffer is full; retransmission limits bound this.
        if (done < len) NET_WAIT(c->tx_count < TCP_TXBUF || c->error || c->state == CLOSED, 30000);
        if (done < len && c->tx_count >= TCP_TXBUF) return c->error ? c->error : NET_ETIMEDOUT;
    }
    return (int)done;
}

int tcp_recv(struct tcp_conn *c, void *buf, size_t len, uint32_t timeout_ms) {
    if (!c) return NET_ERR;
    NET_WAIT(c->rx_count > 0 || c->fin_rcvd || c->error || c->state == CLOSED, timeout_ms);
    if (c->rx_count == 0) {
        if (c->error) return c->error;
        if (c->fin_rcvd) return 0;
        if (c->state == CLOSED) return NET_ECLOSED;
        return NET_ETIMEDOUT;
    }
    size_t n = len < c->rx_count ? len : c->rx_count;
    uint8_t *out = buf;
    for (size_t i = 0; i < n; i++) out[i] = c->rx[(c->rx_head + i) % TCP_RXBUF];
    c->rx_head = (c->rx_head + n) % TCP_RXBUF;
    c->rx_count -= n;
    // Window update if we had (nearly) closed the window.
    if (c->adv_wnd < TCP_MSS && rx_free(c) >= TCP_MSS && c->state != CLOSED) send_ack(c);
    return (int)n;
}

void tcp_close(struct tcp_conn *c) {
    if (!c) return;
    if (c->state == ESTABLISHED || c->state == CLOSE_WAIT) {
        c->fin_queued = true;
        tcp_output(c, false);
        // Wait for our FIN to be acknowledged (and the peer's FIN, if pending).
        NET_WAIT(c->state == CLOSED || c->state == TIME_WAIT || c->state == FIN_WAIT_2, 1000);
        if (c->state == FIN_WAIT_2) NET_WAIT(c->state != FIN_WAIT_2, 500);
    } else if (c->state == SYN_SENT) {
        send_raw(c->rip, c->lport, c->rport, c->snd_nxt, 0, F_RST, 0, 0, 0, false);
    }
    if (c->state != CLOSED && c->state != TIME_WAIT && c->state != FIN_WAIT_2)
        send_seg(c, F_RST, c->snd_nxt, 0, 0);   // abort anything still half-open
    for (int i = 0; i < TCP_MAXCONN; i++) if (conns[i] == c) conns[i] = 0;
    kfree(c);
}
