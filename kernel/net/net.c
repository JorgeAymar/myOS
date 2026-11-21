// Network subsystem: interface state, net_poll() dispatch loop, net_init()
// bring-up (NIC + DHCP) and boot self-tests. Owner: net agent.
#include "net_internal.h"
#include "arch.h"
#include "mm.h"
#include "kprintf.h"
#include "panic.h"
#include "string.h"
#include "boot.h"
#include "proc.h"

struct netif net_if;
bool net_in_poll;

void net_idle(void) {
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
    if (rflags & (1u << 9)) cpu_hlt();          // next timer tick (1 ms) or NIC IRQ
    else __asm__ volatile ("pause");
}

void net_poll(void) {
    if (!net_if.up || net_in_poll) return;
    static uint8_t frame[2048];
    net_in_poll = true;
    for (int budget = 64; budget > 0; budget--) {
        size_t len = e1000_recv(frame, sizeof frame);
        if (!len) break;
        eth_input(frame, len);
    }
    tcp_timers(timer_ticks_ms());
    net_in_poll = false;
}

bool net_ifconfig(struct net_ifconfig *out) {
    memset(out, 0, sizeof *out);
    out->up = net_if.up && e1000_link_up();
    out->configured = net_if.configured;
    memcpy(out->mac, net_if.mac, 6);
    out->ip = net_if.ip; out->mask = net_if.mask;
    out->gw = net_if.gw; out->dns = net_if.dns;
    return net_if.up;
}

char *ip_to_str(uint32_t ip, char *buf) {
    ksnprintf(buf, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
    return buf;
}

bool ip_parse(const char *s, uint32_t *ip) {
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return false;
        uint32_t n = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (uint32_t)(*s++ - '0');
            if (++digits > 3 || n > 255) return false;
        }
        v = (v << 8) | n;
        if (part < 3 && *s++ != '.') return false;
    }
    if (*s) return false;
    *ip = v;
    return true;
}

// ---- boot self-tests ----
#define SELFTEST_HOST "example.com"

// Async job: starts an HTTP job from the boot thread, a second start must be
// refused while it runs, then waits for completion while yielding.
static void job_selftest(void) {
    static int owner;
    if (!net_job_start_http(&owner, SELFTEST_HOST, "/", 2048, 2500)) panic("net-job: start failed");
    if (net_job_start_http(&owner, SELFTEST_HOST, "/", 2048, 2500)) panic("net-job: second start not refused");
    uint64_t dl = timer_ticks_ms() + 6000;
    struct net_job *j;
    while (!(j = net_job_done(&owner)) && timer_ticks_ms() < dl) { net_job_poll(); thread_sleep_ms(1); }
    if (!j) panic("net-job: did not finish");
    int r = j->result;
    net_job_release();
    if (net_job_busy()) panic("net-job: still busy after release");
    kprintf("[OK] net-job async http (result %d), busy-guard ok\n", r);
}

static void net_selftest(void) {
    char a[16];
    uint8_t mac[6];
    uint64_t t0 = timer_ticks_ms();

    if (!net_if.gw) {
        kprintf("[WARN] net: DHCP gave no gateway (restricted network?), skipping self-tests\n");
        return;
    }
    if (!arp_resolve(net_if.gw, mac, 1000))
        panic("net: ARP resolve of gateway %s failed", ip_to_str(net_if.gw, a));
    kprintf("[OK] arp %s is %02x:%02x:%02x:%02x:%02x:%02x\n", ip_to_str(net_if.gw, a),
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    int rtt = net_ping(net_if.gw, 1000);
    if (rtt >= 0) kprintf("[OK] ping %s rtt=%dms\n", ip_to_str(net_if.gw, a), rtt);
    else kprintf("[WARN] ping %s failed (%d)\n", ip_to_str(net_if.gw, a), rtt);

    uint32_t ip;
    int r = dns_resolve(SELFTEST_HOST, &ip, 1500);
    if (r < 0) {
        kprintf("[WARN] dns %s failed (%d): no internet on host? skipping http\n", SELFTEST_HOST, r);
    } else {
        kprintf("[OK] dns %s=%s\n", SELFTEST_HOST, ip_to_str(ip, a));
        static char resp[2048];
        r = http_get_timeout(SELFTEST_HOST, "/", resp, sizeof resp, 2500);
        if (r > 0 && strncmp(resp, "HTTP/", 5) == 0) {
            char line[64];
            size_t i = 0;
            while (i < sizeof line - 1 && resp[i] && resp[i] != '\r' && resp[i] != '\n') {
                line[i] = resp[i]; i++;
            }
            line[i] = 0;
            kprintf("[OK] http GET %s -> %s\n", SELFTEST_HOST, line);
        } else {
            kprintf("[WARN] http GET %s failed (%d)\n", SELFTEST_HOST, r);
        }
    }
    kprintf("[net] self-test took %lums\n", timer_ticks_ms() - t0);
}

void net_init(void) {
    const char *model = "?";
    if (!e1000_init(net_if.mac, &model)) {
        kprintf("[WARN] net: no supported NIC found, networking disabled\n");
        return;
    }
    net_if.up = true;
    uint8_t *m = net_if.mac;
    if (!e1000_link_up()) panic("net: e1000 link down");
    kprintf("[OK] net e1000 mac=%02x:%02x:%02x:%02x:%02x:%02x model=%s link up\n",
            m[0], m[1], m[2], m[3], m[4], m[5], model);

    if (!dhcp_run(4000)) panic("net: DHCP failed");
    char a[16], b[16], c[16], d[16];
    kprintf("[OK] dhcp ip=%s mask=%s gw=%s dns=%s\n", ip_to_str(net_if.ip, a),
            ip_to_str(net_if.mask, b), ip_to_str(net_if.gw, c), ip_to_str(net_if.dns, d));

    // ping/DNS/HTTP go out to the internet and cost ~1.3 s: only in test mode.
    if (boot_is_test()) { net_selftest(); job_selftest(); }
}
