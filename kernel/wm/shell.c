// Desktop shell: frosted menu bar (logo menu, app name, menu titles, status
// glyphs, clock), floating frosted dock with magnification, tooltip, and the
// mouse cursor. All icons are procedural.
#include "wm.h"
#include "rtc.h"
#include "kprintf.h"
#include "string.h"

int cursor_x, cursor_y;

#define WHITE RGB(0xff, 0xff, 0xff)

// ---------------------------------------------------------------- glyphs
// Glyph painters get the icon rect; `u` is 1/16 of the icon side in 1/16 px.
// Coordinates in tenths of those units: GX(85) = 8.5/16 of the width.
#define GX(f) (r.x * 16 + (f) * u / 10)
#define GY(f) (r.y * 16 + (f) * u / 10)

static void glyph_files(struct surface *s, struct rect r) {
    int z = r.w;
    gfx_fill_rrect(s, (struct rect){ r.x + z * 18 / 100, r.y + z * 27 / 100, z * 30 / 100, z * 16 / 100 },
                   z * 4 / 100, RGBA(0xff, 0xff, 0xff, 0xc8));
    gfx_fill_rrect(s, (struct rect){ r.x + z * 18 / 100, r.y + z * 34 / 100, z * 64 / 100, z * 42 / 100 },
                   z * 6 / 100, WHITE);
    gfx_hline(s, r.x + z * 18 / 100, r.y + z * 34 / 100, z * 64 / 100, RGBA(0x0a, 0x60, 0xc8, 0x30));
}

static void glyph_terminal(struct surface *s, struct rect r) {
    int u = r.w, th = u * 9 / 10 < 10 ? 10 : u * 9 / 10;
    gfx_capsule(s, GX(40), GY(50), GX(70), GY(80), th, WHITE);
    gfx_capsule(s, GX(70), GY(80), GX(40), GY(110), th, WHITE);
    gfx_capsule(s, GX(85), GY(110), GX(120), GY(110), th, WHITE);
}

static void glyph_notes(struct surface *s, struct rect r) {
    int z = r.w;
    struct rect p = { r.x + z * 22 / 100, r.y + z * 18 / 100, z * 56 / 100, z * 64 / 100 };
    gfx_fill_rrect(s, p, z * 6 / 100, WHITE);
    for (int i = 0; i < 4; i++)
        gfx_hline(s, p.x + z * 8 / 100, p.y + z * (14 + i * 12) / 100, p.w - z * 16 / 100 - (i == 3 ? z * 14 / 100 : 0),
                  RGB(0xc8, 0xa2, 0x2c));
}

static void glyph_editor(struct surface *s, struct rect r) {
    int z = r.w;
    struct rect p = { r.x + z * 24 / 100, r.y + z * 16 / 100, z * 52 / 100, z * 68 / 100 };
    gfx_fill_rrect(s, p, z * 6 / 100, RGBA(0xff, 0xff, 0xff, 0xf0));
    for (int i = 0; i < 4; i++)
        gfx_hline(s, p.x + z * 8 / 100, p.y + z * (16 + i * 14) / 100, p.w - z * 16 / 100 - (i == 3 ? z * 18 / 100 : 0),
                  RGB(0x8e, 0x8e, 0x93));
    int u = r.w, th = u / 5 > 4 ? u / 5 : 4;
    gfx_capsule(s, GX(104), GY(88), GX(128), GY(64), th, RGB(0xff, 0x9f, 0x0a));
}

static void glyph_clock(struct surface *s, struct rect r) {
    int u = r.w, cx = GX(80), cy = GY(80);
    gfx_disc(s, cx, cy, u * 6, WHITE);
    for (int i = 0; i < 12; i++) {
        int sx = gfx_sin60(i * 5), cs = gfx_cos60(i * 5);
        int ro = u * 5, ri = u * 9 / 2;
        gfx_capsule(s, cx + sx * ro / 16384, cy - cs * ro / 16384, cx + sx * ri / 16384, cy - cs * ri / 16384,
                    u / 6 > 5 ? u / 6 : 5, RGB(0x8e, 0x8e, 0x93));
    }
    int th = u / 4 > 7 ? u / 4 : 7;
    gfx_capsule(s, cx, cy, cx + gfx_sin60(50) * u * 3 / 16384, cy - gfx_cos60(50) * u * 3 / 16384, th, C_TEXT);
    gfx_capsule(s, cx, cy, cx + gfx_sin60(10) * u * 4 / 16384, cy - gfx_cos60(10) * u * 4 / 16384, th * 3 / 4, C_TEXT);
    gfx_capsule(s, cx, cy, cx + gfx_sin60(33) * u * 9 / 2 / 16384, cy - gfx_cos60(33) * u * 9 / 2 / 16384,
                u / 10 > 4 ? u / 10 : 4, RGB(0xff, 0x7a, 0x00));
    gfx_disc(s, cx, cy, u / 3 > 8 ? u / 3 : 8, RGB(0xff, 0x7a, 0x00));
}

static void ellipse(struct surface *s, int cx, int cy, int rx, int ry, int th, color_t c) {
    int px = cx, py = cy - ry;
    for (int a = 32; a <= 1024; a += 32) {
        int x = cx + gfx_sin1024(a) * rx / 16384, y = cy - gfx_cos1024(a) * ry / 16384;
        gfx_capsule(s, px, py, x, y, th, c);
        px = x; py = y;
    }
}

static void glyph_browser(struct surface *s, struct rect r) {
    int u = r.w, cx = GX(80), cy = GY(80), R = u * 11 / 2, th = u / 2 > 9 ? u / 2 : 9;
    gfx_ring(s, cx, cy, R, R - th * 2, WHITE);
    ellipse(s, cx, cy, R * 45 / 100, R - th, th, WHITE);
    gfx_capsule(s, cx, cy - R + th, cx, cy + R - th, th, WHITE);
    gfx_capsule(s, cx - R + th, cy, cx + R - th, cy, th, WHITE);
    int ly = R * 55 / 100, lx = R * 80 / 100;
    gfx_capsule(s, cx - lx, cy - ly, cx + lx, cy - ly, th * 8 / 10, WHITE);
    gfx_capsule(s, cx - lx, cy + ly, cx + lx, cy + ly, th * 8 / 10, WHITE);
}

static void glyph_settings(struct surface *s, struct rect r) {
    int u = r.w, cx = GX(80), cy = GY(80);
    int ro = u * 9 / 2, rt = u * 6;
    for (int i = 0; i < 8; i++) {
        int a = i * 128;
        int sx = gfx_sin1024(a), cs = gfx_cos1024(a);
        gfx_capsule(s, cx + sx * ro / 16384, cy - cs * ro / 16384, cx + sx * (rt - u) / 16384,
                    cy - cs * (rt - u) / 16384, u * 9 / 10, WHITE);
    }
    gfx_ring(s, cx, cy, ro + u / 2, u * 3 / 2, WHITE);
}

static void glyph_info(struct surface *s, struct rect r) {
    int u = r.w, cx = GX(80), th = u * 13 / 10 < 12 ? 12 : u * 13 / 10;
    gfx_disc(s, cx, GY(44), th * 12 / 10, WHITE);
    gfx_capsule(s, cx, GY(74), cx, GY(120), th, WHITE);
}

static void glyph_trash(struct surface *s, struct rect r) {
    int u = r.w, th = u / 2 > 8 ? u / 2 : 8;
    struct rect body = { r.x + r.w * 30 / 100, r.y + r.h * 32 / 100, r.w * 40 / 100, r.h * 48 / 100 };
    gfx_fill_rrect(s, body, r.w * 5 / 100, RGBA(0xff, 0xff, 0xff, 0xf0));
    for (int i = 1; i <= 3; i++) {
        int x = (body.x * 16) + body.w * 16 * i / 4;
        gfx_capsule(s, x, (body.y + 4) * 16, x, (body.y + body.h - 4) * 16, th * 7 / 10, RGB(0x8e, 0x8e, 0x93));
    }
    gfx_capsule(s, GX(42), GY(44), GX(118), GY(44), th * 13 / 10, WHITE);
    gfx_capsule(s, GX(66), GY(33), GX(94), GY(33), th, WHITE);
}

// Tilted elliptical arc from angle a0 to a1 (1/1024 turn), tilt in 1/1024 turn.
static void orbit(struct surface *s, int cx, int cy, int rx, int ry, int tilt, int a0, int a1, int th, color_t c) {
    int ct = gfx_cos1024(tilt), st = gfx_sin1024(tilt), px = 0, py = 0;
    for (int a = a0; a <= a1; a += 32) {
        int ex = gfx_sin1024(a) * rx / 16384, ey = -gfx_cos1024(a) * ry / 16384;
        int x = cx + (ex * ct - ey * st) / 16384, y = cy + (ex * st + ey * ct) / 16384;
        if (a > a0) gfx_capsule(s, px, py, x, y, th, c);
        px = x; py = y;
    }
}

static void glyph_logo(struct surface *s, int cx, int cy, int u, color_t c, color_t gap) {
    // myOS mark: a small planet with a tilted orbit ring passing in front
    int rx = u * 17 / 2, ry = u * 3, tilt = -60;
    orbit(s, cx, cy, rx, ry, tilt, 0, 1024, 11, c);
    gfx_disc(s, cx, cy, u * 5, c);
    orbit(s, cx, cy, rx, ry, tilt, 288, 736, 30, gap);
    orbit(s, cx, cy, rx, ry, tilt, 256, 768, 11, c);
}

static void glyph_welcome(struct surface *s, struct rect r) { glyph_info(s, r); }

void app_icon_paint(struct surface *s, struct rect r, const struct app *a) {
    int radius = r.w * ICON_RADIUS / DOCK_ICON;
    if (radius < 3) radius = 3;
    gfx_gradient_rrect(s, r, radius, a->icon_top, a->icon_bottom);
    gfx_stroke_rrect(s, r, radius, RGBA(0x00, 0x00, 0x00, 0x22));
    if (a->icon_glyph) a->icon_glyph(s, r);
}

// Placeholders until the real apps land (client_w == 0: not openable).
static struct app ph_trash = { .name = "Trash", .icon_top = RGB(0xb4, 0xb4, 0xba),
    .icon_bottom = RGB(0x78, 0x78, 0x7e), .icon_glyph = glyph_trash };

void (*const icon_glyph_welcome)(struct surface *, struct rect) = glyph_welcome;
void (*const icon_glyph_notes)(struct surface *, struct rect) = glyph_notes;
void (*const icon_glyph_clock)(struct surface *, struct rect) = glyph_clock;
void (*const icon_glyph_files)(struct surface *, struct rect) = glyph_files;
void (*const icon_glyph_terminal)(struct surface *, struct rect) = glyph_terminal;
void (*const icon_glyph_editor)(struct surface *, struct rect) = glyph_editor;
void (*const icon_glyph_settings)(struct surface *, struct rect) = glyph_settings;
void (*const icon_glyph_info)(struct surface *, struct rect) = glyph_info;
void (*const icon_glyph_browser)(struct surface *, struct rect) = glyph_browser;

static struct app *dock_apps[] = {
    &app_files, &app_terminal, &app_editor, &app_notes, &app_clock, &app_browser, &app_settings, &app_welcome, &ph_trash,
};
#define NDOCK ((int)(sizeof dock_apps / sizeof dock_apps[0]))
#define DOCK_SEP_BEFORE (NDOCK - 1)     // thin separator before the trash
#define DOCK_SEP_W 17

// ---------------------------------------------------------------- state
static int dock_hover = -1;
static int dock_mouse_x = -1;           // magnification focus, -1 = none
static struct rect dock_icons[NDOCK];   // current layout
static struct rect dock_panel;
static int dock_sep_x;
static int menu_open = -1;              // index into menus[], -1 = closed
static int menu_hover = -1;
static char clock_text[40];
static struct rect clock_drawn;

enum { A_NONE, A_ABOUT, A_MIN, A_MINALL, A_CLOSE, A_CLOSEALL, A_FILES, A_TERMINAL, A_EDITOR };
struct mitem { const char *label; int action; };   // empty label = separator
struct menu { const char *title; const struct mitem *items; int n; };

static const struct mitem logo_items[] = { { "About myOS", A_ABOUT }, { "", 0 }, { "Minimize All", A_MINALL },
                                           { "Close All Windows", A_CLOSEALL } };
static const struct mitem file_items[] = { { "Open Files", A_FILES }, { "Open Terminal", A_TERMINAL },
                                           { "Open Editor", A_EDITOR }, { "", 0 }, { "Close Window", A_CLOSE } };
static const struct mitem window_items[] = { { "Minimize", A_MIN }, { "", 0 }, { "Minimize All", A_MINALL },
                                             { "Close All Windows", A_CLOSEALL } };
static const struct mitem help_items[] = { { "About myOS", A_ABOUT } };
#define NITEMS(a) ((int)(sizeof(a) / sizeof((a)[0])))
// menus[0] is the logo menu; 1.. are the titles in the bar. Edit and View
// have no actions yet, so they do not open.
static const struct menu menus[] = {
    { "", logo_items, NITEMS(logo_items) },
    { "File", file_items, NITEMS(file_items) },
    { "Edit", NULL, 0 },
    { "View", NULL, 0 },
    { "Window", window_items, NITEMS(window_items) },
    { "Help", help_items, NITEMS(help_items) },
};
#define NMENUS NITEMS(menus)

// ---------------------------------------------------------------- dock layout
static int base_width(void) {
    return DOCK_PAD * 2 + NDOCK * DOCK_ICON + (NDOCK - 1) * DOCK_GAP + (DOCK_SEP_W - DOCK_GAP);
}

static void dock_layout(void) {
    int bw = base_width();
    int bx = (scr.w - bw) / 2;
    int sizes[NDOCK], total = 0;
    for (int i = 0; i < NDOCK; i++) {
        int f = 0;
        if (dock_mouse_x >= 0) {
            int c = bx + DOCK_PAD + i * (DOCK_ICON + DOCK_GAP) + DOCK_ICON / 2 + (i >= DOCK_SEP_BEFORE ? DOCK_SEP_W - DOCK_GAP : 0);
            long d = (long)(dock_mouse_x - c) * 256 / (DOCK_ICON + DOCK_GAP);   // slots, 8.8
            long q = 256 - d * d / (256 * 9 / 2);         // 1 - d^2/4.5
            if (q > 0) { q = q * q >> 8; f = (int)(q * q >> 8); }   // ^4, 0..256
        }
        sizes[i] = DOCK_ICON + DOCK_ICON * DOCK_MAG_PCT / 100 * f / 256;
        total += sizes[i];
    }
    int w = DOCK_PAD * 2 + total + (NDOCK - 1) * DOCK_GAP + (DOCK_SEP_W - DOCK_GAP);
    int h = DOCK_ICON + DOCK_PAD * 2;
    dock_panel = (struct rect){ (scr.w - w) / 2, scr.h - DOCK_BOTTOM - h, w, h };
    int x = dock_panel.x + DOCK_PAD, bottom = dock_panel.y + DOCK_PAD + DOCK_ICON;
    for (int i = 0; i < NDOCK; i++) {
        if (i == DOCK_SEP_BEFORE) {
            dock_sep_x = x - DOCK_GAP + DOCK_SEP_W / 2;
            x += DOCK_SEP_W - DOCK_GAP;
        }
        dock_icons[i] = (struct rect){ x, bottom - sizes[i], sizes[i], sizes[i] };
        x += sizes[i] + DOCK_GAP;
    }
}

static struct rect dock_extent(void) {
    // everything the dock may paint: panel, grown icons, tooltip, shadow
    int grow = DOCK_ICON * DOCK_MAG_PCT / 100;
    struct rect r = dock_panel;
    r = rect_inset(r, -18);
    r.y -= grow + 44;
    r.h += grow + 44;
    return r;
}

static void dock_set_mouse(int mx) {
    if (mx == dock_mouse_x) return;
    struct rect before = dock_extent();
    dock_mouse_x = mx;
    dock_layout();
    comp_damage(rect_union(before, dock_extent()));
}

static const char *dock_label(int i) {
    static char buf[48];
    if (dock_apps[i]->client_w || dock_apps[i] == &ph_trash) return dock_apps[i]->name;
    ksnprintf(buf, sizeof buf, "%s " CH_EMDASH " coming soon", dock_apps[i]->name);
    return buf;
}

static struct rect tooltip_rect(int i) {
    int w = text_width(&afont_ui, dock_label(i)) + 22;
    struct rect ic = dock_icons[i];
    return (struct rect){ ic.x + ic.w / 2 - w / 2, ic.y - 34, w, 24 };
}

// ---------------------------------------------------------------- menu bar geometry
static struct rect logo_rect(void) { return (struct rect){ 6, 0, 34, MENUBAR_H }; }

static int sep_h(void) { return 9; }

// x of the left edge of menu title i (1..NMENUS-1) in the bar, and its width.
static struct rect title_rect(int i) {
    struct window *f = wm_focused();
    int x = 48 + text_width(&afont_ui_bold, f ? f->app->name : "myOS") + MENUBAR_ITEM_GAP;
    for (int k = 1; k < i; k++) x += text_width(&afont_ui, menus[k].title) + MENUBAR_ITEM_GAP;
    int w = text_width(&afont_ui, menus[i].title);
    return (struct rect){ x - 8, 2, w + 16, MENUBAR_H - 4 };
}

static struct rect menu_rect(int m) {
    int h = 10;
    for (int k = 0; k < menus[m].n; k++) h += menus[m].items[k].label[0] ? MENU_ITEM_H : sep_h();
    int x = m == 0 ? 8 : title_rect(m).x;
    return (struct rect){ x, MENUBAR_H + 2, MENU_W, h - 1 };
}

static struct rect menu_item_rect(int m, int i) {
    struct rect r = menu_rect(m);
    int y = r.y + 5;
    for (int k = 0; k < i; k++) y += menus[m].items[k].label[0] ? MENU_ITEM_H : sep_h();
    int h = menus[m].items[i].label[0] ? MENU_ITEM_H : sep_h();
    return (struct rect){ r.x + 5, y, r.w - 10, h };
}

struct rect cursor_rect(int x, int y) { return (struct rect){ x - 2, y - 2, 16, 24 }; }

// ---------------------------------------------------------------- clock text
static int weekday(int y, int m, int d) {   // Sakamoto; 0 = Sunday
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d) % 7;
}

void shell_clock_update(void) {
    static const char *const wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const mo[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    struct rtc_time t;
    rtc_read(&t);
    int m = t.month >= 1 && t.month <= 12 ? t.month : 1;
    char buf[40];
    ksnprintf(buf, sizeof buf, "%s %u %s  %02u:%02u",
              wd[weekday(t.year, m, t.day ? t.day : 1)], t.day, mo[m - 1], t.hour, t.minute);
    if (!strcmp(buf, clock_text)) return;
    strcpy(clock_text, buf);
    // clock + status glyphs region
    struct rect r = { scr.w - MENUBAR_PAD - text_width(&afont_ui, clock_text) - 90, 0, 0, MENUBAR_H };
    r.w = scr.w - r.x;
    comp_damage(rect_union(r, clock_drawn));
    clock_drawn = r;
}

void shell_windows_changed(void) {
    comp_damage((struct rect){ 0, 0, scr.w / 2, MENUBAR_H });   // app name
    comp_damage(dock_extent());                                 // running dots
}

void shell_init(void) {
    cursor_x = scr.w / 2 + 180;
    cursor_y = scr.h / 2 + 150;
    dock_layout();
    shell_clock_update();
}

// ---------------------------------------------------------------- drawing
static void draw_status(struct surface *b, int x) {
    int cy = MENUBAR_H / 2;
    // battery
    struct rect bat = { x, cy - 6, 25, 12 };
    gfx_stroke_rrect(b, bat, 4, RGBA(0x1d, 0x1d, 0x1f, 0x90));
    gfx_fill_rrect(b, (struct rect){ bat.x + 2, bat.y + 2, 17, 8 }, 2, C_TEXT);
    gfx_fill_rrect(b, (struct rect){ bat.x + 26, cy - 2, 2, 4 }, 1, RGBA(0x1d, 0x1d, 0x1f, 0x90));
    // wifi: three arcs and a dot
    int wx = (x - 22) * 16 + 8, wy = (cy + 6) * 16;
    gfx_arc_up(b, wx, wy, 11 * 16, 9 * 16 - 4, C_TEXT);
    gfx_arc_up(b, wx, wy, 7 * 16 + 8, 5 * 16 + 6, C_TEXT);
    gfx_arc_up(b, wx, wy, 4 * 16, 0, C_TEXT);
}

static void draw_menubar(struct surface *b) {
    struct rect bar = { 0, 0, scr.w, MENUBAR_H };
    if (!rect_overlaps(bar, b->clip)) return;
    gfx_blit(b, 0, 0, &scr.frost_top);
    gfx_hline(b, 0, MENUBAR_H - 1, scr.w, MENUBAR_LINE);
    int ty = (MENUBAR_H - afont_ui.height) / 2;
    if (menu_open == 0) gfx_fill_rrect(b, (struct rect){ 8, 2, 30, MENUBAR_H - 4 }, 5, MENUBAR_SEL);
    else if (menu_open > 0) gfx_fill_rrect(b, title_rect(menu_open), 5, MENUBAR_SEL);
    glyph_logo(b, 23 * 16, MENUBAR_H * 8, 16, C_TEXT, menu_open == 0 ? RGB(0xd0, 0xd0, 0xd6) : RGB(0xee, 0xec, 0xf6));
    struct window *f = wm_focused();
    int x = 48;
    x = text_draw(b, &afont_ui_bold, x, ty, f ? f->app->name : "myOS", C_TEXT) + MENUBAR_ITEM_GAP;
    for (int i = 1; i < (int)NMENUS; i++)
        x = text_draw(b, &afont_ui, x, ty, menus[i].title, C_TEXT) + MENUBAR_ITEM_GAP;
    int cw = text_width(&afont_ui, clock_text);
    text_draw(b, &afont_ui, scr.w - MENUBAR_PAD - cw, ty, clock_text, C_TEXT);
    draw_status(b, scr.w - MENUBAR_PAD - cw - 44);
}

static void draw_dock(struct surface *b) {
    if (!rect_overlaps(dock_extent(), b->clip)) return;
    struct rect d = dock_panel;
    gfx_shadow(b, d, DOCK_RADIUS, 16, 4, 0x38);
    struct surface frost;
    gfx_surface_sub(&scr.frost_dock, &frost, (struct rect){ d.x, d.y - scr.frost_dock_y, d.w, d.h });
    gfx_blit_rounded(b, d.x, d.y, &frost, DOCK_RADIUS);
    gfx_stroke_rrect(b, rect_inset(d, -1), DOCK_RADIUS + 1, DOCK_OUTLINE);
    gfx_stroke_rrect(b, d, DOCK_RADIUS, DOCK_BORDER);
    gfx_vline(b, dock_sep_x, d.y + 10, d.h - 20, DOCK_SEP);
    for (int i = 0; i < NDOCK; i++) {
        struct rect ic = dock_icons[i];
        if (!rect_overlaps(ic, b->clip) && !rect_overlaps(rect_inset(ic, -4), b->clip)) continue;
        app_icon_paint(b, ic, dock_apps[i]);
        if (i == dock_hover && !dock_apps[i]->client_w && dock_apps[i] != &ph_trash)
            gfx_fill_rrect(b, ic, ic.w * ICON_RADIUS / DOCK_ICON, RGBA(0xff, 0xff, 0xff, 0x30));
        struct window *w = dock_apps[i]->win;
        if (w && w->open)
            gfx_disc(b, (ic.x * 2 + ic.w) * 8, (d.y + d.h - 3) * 16, 2 * 16, DOCK_DOT);
    }
}

static void draw_tooltip(struct surface *b) {
    if (dock_hover < 0) return;
    struct rect r = tooltip_rect(dock_hover);
    if (!rect_overlaps(rect_inset(r, -12), b->clip)) return;
    gfx_shadow(b, r, 6, 8, 2, 0x40);
    gfx_fill_rrect(b, r, 6, TOOLTIP_BG);
    gfx_stroke_rrect(b, r, 6, MENU_BORDER);
    text_center(b, &afont_ui, r.x, r.w, r.y + (r.h - afont_ui.height) / 2, dock_label(dock_hover), C_TEXT);
}

static void draw_menu(struct surface *b) {
    if (menu_open < 0) return;
    struct rect m = menu_rect(menu_open);
    if (!rect_overlaps(rect_inset(m, -24), b->clip)) return;
    gfx_shadow(b, m, MENU_RADIUS, 18, 8, 0x48);
    gfx_fill_rrect(b, m, MENU_RADIUS, MENU_BG);
    gfx_stroke_rrect(b, m, MENU_RADIUS, MENU_BORDER);
    for (int i = 0; i < menus[menu_open].n; i++) {
        struct rect r = menu_item_rect(menu_open, i);
        const char *label = menus[menu_open].items[i].label;
        if (!label[0]) { gfx_hline(b, r.x + 6, r.y + r.h / 2, r.w - 12, MENU_BORDER); continue; }
        bool hov = i == menu_hover;
        if (hov) gfx_fill_rrect(b, r, 5, MENU_HOVER);
        text_draw(b, &afont_ui, r.x + 10, r.y + (r.h - afont_ui.height) / 2, label, hov ? WHITE : C_TEXT);
    }
}

void shell_draw(struct surface *b) {
    draw_menubar(b);
    draw_dock(b);
    draw_tooltip(b);
    draw_menu(b);
}

static const char *const arrow[] = {
    ".",
    "..",
    ".#.",
    ".##.",
    ".###.",
    ".####.",
    ".#####.",
    ".######.",
    ".#######.",
    ".########.",
    ".#########.",
    ".##########.",
    ".######.....",
    ".###.##.",
    ".##..##.",
    ".#. .##.",
    "..   .##.",
    ".    .##.",
    "      .##.",
    "      .##.",
    "       ..",
};

void shell_draw_cursor(struct surface *b) {
    gfx_draw_mask(b, cursor_x, cursor_y, arrow, (int)(sizeof arrow / sizeof arrow[0]),
                  CURSOR_FG, CURSOR_OUTLINE);
}

// ---------------------------------------------------------------- input
static struct rect menu_damage_rect(int m) {
    if (m < 0) return (struct rect){ 0, 0, 0, 0 };
    return rect_inset(menu_rect(m), -30);
}

static void set_menu(int m) {
    if (menu_open == m) return;
    comp_damage(menu_damage_rect(menu_open));
    comp_damage(menu_damage_rect(m));
    comp_damage((struct rect){ 0, 0, scr.w / 2, MENUBAR_H });   // highlighted title
    menu_open = m;
    menu_hover = -1;
}

static void menu_action(int action) {
    struct window *f = wm_focused();
    switch (action) {
    case A_ABOUT: wm_open(&app_about); break;
    case A_MIN: if (f) wm_minimize(f); break;
    case A_MINALL: for (int k = nwindows - 1; k >= 0; k--) wm_minimize(zorder[k]); break;
    case A_CLOSE: if (f) wm_close(f); break;
    case A_CLOSEALL: while (nwindows) wm_close(zorder[nwindows - 1]); break;
    case A_FILES: wm_open(&app_files); break;
    case A_TERMINAL: wm_open(&app_terminal); break;
    case A_EDITOR: wm_open(&app_editor); break;
    }
}

// Which menu (0 = logo, 1.. = titles) the bar position (x, y) belongs to, or -1.
static int menu_at_bar(int x, int y) {
    if (y >= MENUBAR_H) return -1;
    if (rect_contains(logo_rect(), x, y)) return 0;
    for (int i = 1; i < (int)NMENUS; i++)
        if (menus[i].n && rect_contains(title_rect(i), x, y)) return i;
    return -1;
}

static int dock_hit(int x, int y) {
    if (!rect_contains(rect_inset(dock_panel, -2), x, y) && y < dock_panel.y - DOCK_ICON) return -1;
    for (int i = 0; i < NDOCK; i++) {
        struct rect r = dock_icons[i];
        r.x -= DOCK_GAP / 2; r.w += DOCK_GAP;
        r.h = dock_panel.y + dock_panel.h - r.y;
        if (rect_contains(r, x, y)) return i;
    }
    return -1;
}

bool shell_mouse_down(int x, int y) {
    if (menu_open >= 0) {
        if (rect_contains(menu_rect(menu_open), x, y)) {
            int m = menu_open;
            for (int i = 0; i < menus[m].n; i++)
                if (menus[m].items[i].label[0] && rect_contains(menu_item_rect(m, i), x, y)) {
                    set_menu(-1);
                    menu_action(menus[m].items[i].action);
                    break;
                }
            return true;
        }
        int hit = menu_at_bar(x, y);
        set_menu(hit == menu_open ? -1 : hit);
        if (y < MENUBAR_H) return true;
    }
    if (y < MENUBAR_H) {
        int hit = menu_at_bar(x, y);
        if (hit >= 0) set_menu(hit);
        return true;
    }
    int i = dock_hit(x, y);
    if (i >= 0 || rect_contains(dock_panel, x, y)) {
        if (i >= 0 && dock_apps[i]->client_w) {
            struct app *a = dock_apps[i];
            struct window *w = a->win;
            if (w && w->open && !w->minimized && wm_focused() == w) { /* already in front */ }
            else wm_open(a);
        }
        return true;
    }
    return false;
}

void shell_mouse_move(int x, int y) {
    bool near = rect_contains(rect_inset(dock_panel, -6), x, y) ||
                (y >= dock_panel.y - DOCK_ICON * DOCK_MAG_PCT / 100 && y < scr.h &&
                 x >= dock_panel.x && x < dock_panel.x + dock_panel.w);
    dock_set_mouse(near ? x : -1);
    int h = near ? dock_hit(x, y) : -1;
    if (h != dock_hover) {
        comp_damage(dock_extent());
        dock_hover = h;
    }
    if (menu_open >= 0) {
        int mh = -1, m = menu_open;
        for (int i = 0; i < menus[m].n; i++)
            if (menus[m].items[i].label[0] && rect_contains(menu_item_rect(m, i), x, y)) mh = i;
        if (mh != menu_hover) {
            if (menu_hover >= 0) comp_damage(menu_item_rect(m, menu_hover));
            if (mh >= 0) comp_damage(menu_item_rect(m, mh));
            menu_hover = mh;
        }
        int hit = menu_at_bar(x, y);        // slide between titles while open
        if (hit > 0 && hit != m) set_menu(hit);
    }
}
