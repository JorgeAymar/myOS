// "Welcome to myOS": the first sample window. Intro text, a live uptime
// footer (repainted once per second, only that strip is damaged) and a
// "Get Started" button that closes the window.
#include "../wm/wm.h"
#include "arch.h"
#include "kprintf.h"

#define PAD 32
#define FOOT_Y 262

static struct rect button_r(struct window *w) {
    return (struct rect){ w->client.w - PAD - 120, FOOT_Y - 6, 120, 28 };
}

static void paint_footer(struct window *w) {
    struct surface *s = &w->client;
    struct rect r = { PAD, FOOT_Y - 6, s->w - 2 * PAD - 130, 28 };
    gfx_reset_clip(s);
    gfx_fill(s, r, WIN_BG);
    uint64_t sec = timer_ticks_ms() / 1000;
    char buf[80];
    ksnprintf(buf, sizeof buf, "Uptime %lu:%02lu:%02lu   " CH_MIDDOT "   Display %dx%d",
              sec / 3600, (sec / 60) % 60, sec % 60, scr.w, scr.h);
    text_draw(s, &afont_small, PAD, FOOT_Y + 1, buf, C_TEXT_3);
    wm_invalidate(w, r);
}

static void paint(struct window *w) {
    struct surface *s = &w->client;
    gfx_reset_clip(s);
    gfx_fill(s, (struct rect){ 0, 0, s->w, s->h }, WIN_BG);

    // header: app icon, title, version
    struct rect ic = { PAD, 28, 64, 64 };
    app_icon_paint(s, ic, w->app);
    text_draw(s, &afont_title, PAD + 84, 32, "Welcome to myOS", C_TEXT);
    text_draw(s, &afont_ui, PAD + 85, 66, "Version 0.1  " CH_MIDDOT "  x86_64", C_TEXT_3);

    static const char *const para[] = {
        "myOS is a hobby operating system written from scratch in C:",
        "its own kernel, compositor, window manager and desktop.",
        "",
        CH_BULLET "  Drag a window by its title bar to move it.",
        CH_BULLET "  Click a window to bring it to the front.",
        CH_BULLET "  Use the lights at the top left to close or minimize.",
    };
    int y = 120;
    for (unsigned i = 0; i < sizeof para / sizeof para[0]; i++) {
        if (!para[i][0]) { y += 8; continue; }
        text_draw(s, &afont_ui, PAD, y, para[i], i < 2 ? C_TEXT : C_TEXT_2);
        y += 20;
    }
    gfx_hline(s, PAD, FOOT_Y - 18, s->w - 2 * PAD, C_SEPARATOR);

    struct rect b = button_r(w);
    gfx_gradient_rrect(s, b, 7, RGB(0x2b, 0x8c, 0xff), C_ACCENT);
    text_center(s, &afont_ui_med, b.x, b.w, b.y + (b.h - afont_ui_med.height) / 2, "Get Started",
                RGB(0xff, 0xff, 0xff));
    wm_invalidate_all(w);
    paint_footer(w);
}

static void mouse(struct window *w, int x, int y, uint8_t buttons, int wheel) {
    (void)wheel;
    if ((buttons & MOUSE_LEFT) && rect_contains(button_r(w), x, y)) wm_close(w);
}

static void tick(struct window *w, uint64_t now, bool new_second) {
    (void)now;
    if (new_second) paint_footer(w);
}

static void glyph(struct surface *s, struct rect r) { icon_glyph_welcome(s, r); }

struct app app_welcome = {
    .name = "Welcome",
    .client_w = 560, .client_h = 300,
    .icon_top = RGB(0xd0, 0x72, 0xe8), .icon_bottom = RGB(0x6a, 0x4c, 0xf6),
    .icon_glyph = glyph,
    .paint = paint,
    .mouse = mouse,
    .tick = tick,
};
