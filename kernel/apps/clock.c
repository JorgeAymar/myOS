// "Clock": analog face with hour/minute/second hands, digital time and date.
// Repaints once per second (driven by the app tick from the desktop loop).
#include "../wm/wm.h"
#include "rtc.h"
#include "kprintf.h"

#define FACE_R   118          // face radius in pixels
#define FACE_CY  146          // face centre y (client coords)
#define S(v)     ((v) * SUBPX) // pixels -> 1/16 px

#define C_FACE      RGB(0xfb, 0xfb, 0xfd)
#define C_RIM       RGB(0xd6, 0xd6, 0xdc)
#define C_TICK      RGB(0x8e, 0x8e, 0x93)
#define C_HAND      RGB(0x1d, 0x1d, 0x1f)
#define C_SECOND    RGB(0xff, 0x7a, 0x00)

static const char *const weekdays[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const months[] = {
    "January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December" };

// Point at angle a (1024 steps per turn, 0 = 12 o'clock) and radius r (1/16 px).
static int px_at(int cx, int a, int r) { return cx + (int)((int64_t)gfx_sin1024(a) * r / 16384); }
static int py_at(int cy, int a, int r) { return cy - (int)((int64_t)gfx_cos1024(a) * r / 16384); }

// A hand from slightly behind the centre (tail) to length `len`, all 1/16 px.
static void hand(struct surface *s, int cx, int cy, int a, int len, int tail, int th, color_t c) {
    int ta = (a + 512) & 1023;
    gfx_capsule(s, px_at(cx, ta, tail), py_at(cy, ta, tail), px_at(cx, a, len), py_at(cy, a, len), th, c);
}

static void paint(struct window *w) {
    struct surface *s = &w->client;
    gfx_reset_clip(s);
    gfx_fill(s, (struct rect){ 0, 0, s->w, s->h }, WIN_BG);

    struct rtc_time t;
    rtc_read(&t);

    int cx = S(s->w / 2), cy = S(FACE_CY);

    // face: soft outer rim, white dial
    gfx_disc(s, cx, cy, S(FACE_R + 4), C_RIM);
    gfx_disc(s, cx, cy, S(FACE_R), C_FACE);

    // minute and hour ticks
    for (int i = 0; i < 60; i++) {
        bool hour = (i % 5) == 0;
        int ro = S(FACE_R - 8), ri = hour ? S(FACE_R - 22) : S(FACE_R - 13);
        int sx = gfx_sin60(i), cs = gfx_cos60(i);
        gfx_capsule(s, cx + sx * ro / 16384, cy - cs * ro / 16384,
                    cx + sx * ri / 16384, cy - cs * ri / 16384,
                    hour ? 24 : 10, hour ? C_HAND : C_TICK);
    }

    // numerals 1..12
    for (int n = 1; n <= 12; n++) {
        char num[3];
        ksnprintf(num, sizeof num, "%d", n);
        int r = FACE_R - 40;
        int x = s->w / 2 + gfx_sin60(n * 5) * r / 16384;
        int y = FACE_CY - gfx_cos60(n * 5) * r / 16384;
        int tw = text_width(&afont_ui_med, num);
        text_draw(s, &afont_ui_med, x - tw / 2, y - afont_ui_med.height / 2, num, C_HAND);
    }

    // hands (angles in 1/1024 turn)
    int h12 = t.hour % 12;
    int a_hour = (h12 * 3600 + t.minute * 60 + t.second) * 1024 / 43200;
    int a_min  = (t.minute * 60 + t.second) * 1024 / 3600;
    int a_sec  = t.second * 1024 / 60;
    hand(s, cx, cy, a_hour, S(FACE_R * 52 / 100), S(10), 46, C_HAND);
    hand(s, cx, cy, a_min,  S(FACE_R * 78 / 100), S(12), 34, C_HAND);
    gfx_disc(s, cx, cy, S(6), C_HAND);
    hand(s, cx, cy, a_sec,  S(FACE_R * 86 / 100), S(22), 14, C_SECOND);
    gfx_disc(s, cx, cy, S(4), C_SECOND);
    gfx_disc(s, cx, cy, S(2), C_FACE);

    // digital time + date
    char buf[64];
    ksnprintf(buf, sizeof buf, "%02u:%02u:%02u", t.hour, t.minute, t.second);
    text_center(s, &afont_title, 0, s->w, FACE_CY + FACE_R + 22, buf, C_TEXT);
    const char *wd = t.weekday < 7 ? weekdays[t.weekday] : "";
    const char *mo = (t.month >= 1 && t.month <= 12) ? months[t.month - 1] : "";
    ksnprintf(buf, sizeof buf, "%s, %u %s %u", wd, t.day, mo, t.year);
    text_center(s, &afont_ui, 0, s->w, FACE_CY + FACE_R + 58, buf, C_TEXT_3);

    wm_invalidate_all(w);
}

static void tick(struct window *w, uint64_t now_ms, bool new_second) {
    (void)now_ms;
    if (new_second && !w->minimized) paint(w);
}

static void glyph(struct surface *s, struct rect r) { icon_glyph_clock(s, r); }

struct app app_clock = {
    .name = "Clock",
    .client_w = 300, .client_h = 360,
    .icon_top = RGB(0x3a, 0x3a, 0x3e), .icon_bottom = RGB(0x12, 0x12, 0x14),
    .icon_glyph = glyph,
    .paint = paint,
    .tick = tick,
};
