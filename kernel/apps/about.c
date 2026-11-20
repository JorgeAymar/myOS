// "About myOS": logo, version, build info, memory, uptime and display.
// Uptime is refreshed once per second from the app tick.
#include "../wm/wm.h"
#include "arch.h"
#include "mm.h"
#include "kprintf.h"

#define PAD 24
#define S(v) ((v) * SUBPX)

#define C_GROUP RGB(0xf2, 0xf2, 0xf7)

// Logo glyph: a white ring around a solid core, on the accent gradient.
static void glyph(struct surface *s, struct rect r) {
    int cx = S(r.x) + S(r.w) / 2, cy = S(r.y) + S(r.h) / 2;
    int u = S(r.w) / 100;
    gfx_ring(s, cx, cy, 30 * u, 22 * u, RGB(0xff, 0xff, 0xff));
    gfx_disc(s, cx, cy, 11 * u, RGB(0xff, 0xff, 0xff));
}

static void row(struct surface *s, int x, int y, int w, const char *label, const char *val) {
    gfx_set_clip(s, (struct rect){ x, y, w, 24 });
    text_draw(s, &afont_ui, x + 12, y + 4, label, C_TEXT_2);
    int vw = text_width(&afont_ui, val);
    int vx = x + w - 12 - vw;
    if (vx < x + 110) vx = x + 110;
    text_draw(s, &afont_ui, vx, y + 4, val, C_TEXT);
    gfx_reset_clip(s);
}

static void paint(struct window *w) {
    struct surface *s = &w->client;
    gfx_reset_clip(s);
    gfx_fill(s, (struct rect){ 0, 0, s->w, s->h }, WIN_BG);

    struct rect ic = { PAD, 24, 72, 72 };
    app_icon_paint(s, ic, w->app);
    text_draw(s, &afont_title, PAD + 92, 34, "myOS", C_TEXT);
    text_draw(s, &afont_ui, PAD + 93, 66, "Version 0.1", C_TEXT_3);

    int gx = PAD, gw = s->w - 2 * PAD, gy = 112;
    char buf[80];
    gfx_fill_rrect(s, (struct rect){ gx, gy, gw, 5 * 24 }, 8, C_GROUP);
    for (int i = 1; i < 5; i++) gfx_hline(s, gx + 12, gy + i * 24, gw - 12, C_SEPARATOR);

    row(s, gx, gy, gw, "Build", "x86_64, Limine boot");

    uint64_t total = pmm_total_bytes(), freeb = pmm_free_bytes();
    uint64_t used = total > freeb ? total - freeb : 0;
    ksnprintf(buf, sizeof buf, "%lu MB used / %lu MB", used >> 20, total >> 20);
    row(s, gx, gy + 24, gw, "Memory", buf);

    uint64_t sec = timer_ticks_ms() / 1000;
    ksnprintf(buf, sizeof buf, "%lu:%02lu:%02lu", sec / 3600, (sec / 60) % 60, sec % 60);
    row(s, gx, gy + 48, gw, "Uptime", buf);

    ksnprintf(buf, sizeof buf, "%d x %d", scr.w, scr.h);
    row(s, gx, gy + 72, gw, "Display", buf);

    row(s, gx, gy + 96, gw, "Kernel", "myOS, higher half");

    text_center(s, &afont_small, 0, s->w, s->h - 44, "A hobby operating system written from scratch.", C_TEXT_3);
    text_center(s, &afont_small, 0, s->w, s->h - 28, "C, NASM and a lot of curiosity.", C_TEXT_3);
    wm_invalidate_all(w);
}

static void init(struct window *w) { (void)w; kprintf("[OK] about\n"); }

static void tick(struct window *w, uint64_t now_ms, bool new_second) {
    (void)now_ms;
    if (new_second && !w->minimized) paint(w);
}

struct app app_about = {
    .name = "About myOS",
    .client_w = 360, .client_h = 300,
    .icon_top = RGB(0x2b, 0x8c, 0xff), .icon_bottom = RGB(0x00, 0x5a, 0xd8),
    .icon_glyph = glyph,
    .init = init,
    .paint = paint,
    .tick = tick,
};
