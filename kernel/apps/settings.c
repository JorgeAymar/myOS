// "Settings": read-only overview of system, network and storage.
// Two columns of grouped rows; refreshed once per second from the app tick.
#include "../wm/wm.h"
#include "arch.h"
#include "mm.h"
#include "net.h"
#include "vfs.h"
#include "blockdev.h"
#include "kprintf.h"

#define PAD   20
#define ROW_H 24
#define GAP   16
#define C_GROUP RGB(0xf2, 0xf2, 0xf7)

static char cpu_vendor[13];

static void cpu_probe(void) {
    uint32_t a = 0, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    for (int i = 0; i < 4; i++) {
        cpu_vendor[i]     = (char)(b >> (8 * i));
        cpu_vendor[4 + i] = (char)(d >> (8 * i));
        cpu_vendor[8 + i] = (char)(c >> (8 * i));
    }
    cpu_vendor[12] = 0;
    for (int i = 0; i < 12; i++)
        if (cpu_vendor[i] < 32 || cpu_vendor[i] > 126) cpu_vendor[i] = '?';
}

static void glyph(struct surface *s, struct rect r) {
    int cx = r.x * SUBPX + r.w * SUBPX / 2, cy = r.y * SUBPX + r.h * SUBPX / 2;
    int u = r.w * SUBPX / 100;
    for (int i = 0; i < 8; i++) {
        int sx = gfx_sin1024(i * 128), cs = gfx_cos1024(i * 128);
        gfx_capsule(s, cx + sx * 24 * u / 16384, cy - cs * 24 * u / 16384,
                    cx + sx * 34 * u / 16384, cy - cs * 34 * u / 16384, 8 * u, RGB(0xff, 0xff, 0xff));
    }
    gfx_ring(s, cx, cy, 30 * u, 14 * u, RGB(0xff, 0xff, 0xff));
}

static void heading(struct surface *s, int x, int y, const char *t) {
    text_draw(s, &afont_ui_bold, x + 4, y, t, C_TEXT_2);
}

// A grouped box of rows; returns the y below it.
struct kv { const char *k; const char *v; };
static int group(struct surface *s, int x, int y, int w, const char *title, const struct kv *rows, int n) {
    heading(s, x, y, title);
    y += 20;
    gfx_fill_rrect(s, (struct rect){ x, y, w, n * ROW_H }, 8, C_GROUP);
    for (int i = 0; i < n; i++) {
        int ry = y + i * ROW_H;
        if (i) gfx_hline(s, x + 12, ry, w - 12, C_SEPARATOR);
        gfx_set_clip(s, (struct rect){ x, ry, w, ROW_H });
        text_draw(s, &afont_ui, x + 12, ry + 4, rows[i].k, C_TEXT_2);
        int vw = text_width(&afont_ui, rows[i].v);
        int vx = x + w - 12 - vw;
        if (vx < x + 80) vx = x + 80;
        text_draw(s, &afont_ui, vx, ry + 4, rows[i].v, C_TEXT);
        gfx_reset_clip(s);
    }
    return y + n * ROW_H + GAP;
}

static void paint(struct window *w) {
    struct surface *s = &w->client;
    gfx_reset_clip(s);
    gfx_fill(s, (struct rect){ 0, 0, s->w, s->h }, WIN_BG);

    int colw = (s->w - 2 * PAD - GAP) / 2;
    int lx = PAD, rx = PAD + colw + GAP;
    char res[24], mem[40], up[24], iface[40];
    char ip[16], mask[16], gw[16], dns[16], mac[20];

    // System
    ksnprintf(res, sizeof res, "%d x %d", scr.w, scr.h);
    uint64_t total = pmm_total_bytes(), freeb = pmm_free_bytes();
    uint64_t used = total > freeb ? total - freeb : 0;
    ksnprintf(mem, sizeof mem, "%lu / %lu MB", used >> 20, total >> 20);
    uint64_t sec = timer_ticks_ms() / 1000;
    ksnprintf(up, sizeof up, "%lu:%02lu:%02lu", sec / 3600, (sec / 60) % 60, sec % 60);
    struct kv sys[] = {
        { "Display", res }, { "Memory used", mem }, { "Uptime", up },
        { "CPU", cpu_vendor }, { "Appearance", "Light" },
    };
    int y = group(s, lx, PAD - 4, colw, "System", sys, 5);

    // Storage
    struct vfs_stat st;
    bool disk = vfs_stat("/disk", &st) == 0;
    char bd[16];
    ksnprintf(bd, sizeof bd, "%d", blockdev_count());
    struct kv sto[] = { { "Block devices", bd }, { "Disk (/disk)", disk ? "Mounted" : "Not present" } };
    group(s, lx, y, colw, "Storage", sto, 2);

    // Network
    struct net_ifconfig nc;
    bool have = net_ifconfig(&nc);
    if (have) {
        ip_to_str(nc.ip, ip); ip_to_str(nc.mask, mask);
        ip_to_str(nc.gw, gw); ip_to_str(nc.dns, dns);
        ksnprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x",
                  nc.mac[0], nc.mac[1], nc.mac[2], nc.mac[3], nc.mac[4], nc.mac[5]);
        ksnprintf(iface, sizeof iface, "%s", !nc.up ? "Link down" : nc.configured ? "Connected" : "No address");
    } else {
        ksnprintf(iface, sizeof iface, "No network adapter");
        ksnprintf(ip, sizeof ip, "-"); ksnprintf(mask, sizeof mask, "-");
        ksnprintf(gw, sizeof gw, "-"); ksnprintf(dns, sizeof dns, "-");
        ksnprintf(mac, sizeof mac, "-");
    }
    struct kv net[] = {
        { "Status", iface }, { "IP address", ip }, { "Subnet mask", mask },
        { "Router", gw }, { "DNS", dns }, { "MAC", mac },
    };
    group(s, rx, PAD - 4, colw, "Network", net, 6);

    wm_invalidate_all(w);
}

static void init(struct window *w) {
    (void)w;
    cpu_probe();
    kprintf("[OK] settings\n");
}

static void tick(struct window *w, uint64_t now_ms, bool new_second) {
    (void)now_ms;
    if (new_second && !w->minimized) paint(w);
}

struct app app_settings = {
    .name = "Settings",
    .client_w = 520, .client_h = 330,
    .icon_top = RGB(0x9a, 0x9a, 0xa0), .icon_bottom = RGB(0x5e, 0x5e, 0x64),
    .icon_glyph = glyph,
    .init = init,
    .paint = paint,
    .tick = tick,
};
