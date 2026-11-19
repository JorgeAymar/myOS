// "Notes": a small static sample window (shows inactive window styling
// behind the welcome window until real apps exist).
#include "../wm/wm.h"

static void paint(struct window *w) {
    struct surface *s = &w->client;
    gfx_reset_clip(s);
    gfx_fill(s, (struct rect){ 0, 0, s->w, s->h }, WIN_BG);
    text_draw(s, &afont_ui_bold, 22, 18, "Roadmap", C_TEXT);
    text_draw(s, &afont_small, 22, 38, "Today", C_TEXT_3);
    static const char *const items[] = {
        "Terminal with a tiny shell",
        "Analog clock",
        "About this computer",
        "Files and a text editor",
    };
    int y = 66;
    for (unsigned i = 0; i < sizeof items / sizeof items[0]; i++, y += 26) {
        gfx_ring(s, 28 * 16, (y + 8) * 16, 6 * 16, 5 * 16 - 4, C_TEXT_INACTIVE);
        text_draw(s, &afont_ui, 44, y, items[i], C_TEXT);
        gfx_hline(s, 44, y + 21, s->w - 66, C_SEPARATOR);
    }
    wm_invalidate_all(w);
}

static void glyph(struct surface *s, struct rect r) { icon_glyph_notes(s, r); }

struct app app_notes = {
    .name = "Notes",
    .client_w = 340, .client_h = 196,
    .icon_top = RGB(0xff, 0xe0, 0x7a), .icon_bottom = RGB(0xff, 0xbf, 0x2a),
    .icon_glyph = glyph,
    .paint = paint,
};
