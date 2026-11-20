// Windows: creation, z-order, focus, decorations, drawing and damage.
#include "wm.h"
#include "kprintf.h"

struct window *zorder[MAX_WINDOWS];
int nwindows;

static struct window pool[MAX_WINDOWS];
static int npool;
static int cascade;

struct rect wm_bounds(struct window *w) {
    struct rect r = rect_inset(w->frame, -WIN_SHADOW);
    r.h += WIN_SHADOW_Y;
    return r;
}

static struct rect bounds_of(struct rect frame) {
    struct rect r = rect_inset(frame, -WIN_SHADOW);
    r.h += WIN_SHADOW_Y;
    return r;
}

struct window *wm_focused(void) {
    for (int i = nwindows - 1; i >= 0; i--)
        if (zorder[i]->open && !zorder[i]->minimized) return zorder[i];
    return 0;
}

static void zorder_remove(struct window *w) {
    for (int i = 0; i < nwindows; i++)
        if (zorder[i] == w) {
            for (int j = i; j < nwindows - 1; j++) zorder[j] = zorder[j + 1];
            nwindows--;
            return;
        }
}

// Repaint title bars whose focus state changed, notify apps.
static void refresh_focus(void) {
    struct window *f = wm_focused();
    for (int i = 0; i < nwindows; i++) {
        struct window *w = zorder[i];
        bool focused = (w == f);
        if (w->open && !w->minimized && w->drawn_focused != focused) {
            w->drawn_focused = focused;
            wm_paint_titlebar(w);
            comp_damage(wm_bounds(w));   // shadow strength + border change too
            if (w->app->focus) w->app->focus(w, focused);
        }
    }
    shell_windows_changed();
}

void wm_focus(struct window *w) {
    if (wm_focused() == w) return;
    zorder_remove(w);
    zorder[nwindows++] = w;
    comp_damage(wm_bounds(w));
    refresh_focus();
}

static struct window *create(struct app *a) {
    if (npool == MAX_WINDOWS) return 0;
    struct window *w = &pool[npool];
    int fw = a->client_w, fh = a->client_h + WIN_TITLE_H;
    if (!gfx_surface_alloc(&w->surf, fw, fh)) {
        kprintf("[WARN] desktop: no memory for window '%s'\n", a->name);
        return 0;
    }
    npool++;
    w->app = a;
    a->win = w;
    // Cascade new windows from the upper-left quadrant.
    int x = 80 + cascade * 36, y = MENUBAR_H + 40 + cascade * 30;
    cascade = (cascade + 1) % 6;
    if (x + fw > scr.w) x = scr.w - fw > 0 ? scr.w - fw : 0;
    if (y + fh > scr.h) y = MENUBAR_H;
    w->frame = (struct rect){ x, y, fw, fh };
    w->drawn = w->frame;
    gfx_surface_sub(&w->surf, &w->client, (struct rect){ 0, WIN_TITLE_H, fw, a->client_h });
    gfx_fill(&w->surf, (struct rect){ 0, 0, fw, fh }, WIN_BG);
    if (a->init) a->init(w);
    return w;
}

struct window *wm_open(struct app *a) {
    struct window *w = a->win;
    if (!w && !(w = create(a))) return 0;
    if (!w->open) {
        w->open = true;
        w->minimized = false;
        w->drawn_focused = false;
        w->hover_btn = w->press_btn = BTN_NONE;
        zorder[nwindows++] = w;
        wm_paint_titlebar(w);
        if (a->paint) a->paint(w);
    } else if (w->minimized) {
        w->minimized = false;
    }
    w->drawn = w->frame;
    comp_damage(wm_bounds(w));
    zorder_remove(w);
    zorder[nwindows++] = w;
    refresh_focus();
    return w;
}

void wm_close(struct window *w) {
    if (!w->open) return;
    comp_damage(wm_bounds(w));
    w->open = false;
    if (w->app->focus && w->drawn_focused) w->app->focus(w, false);
    w->drawn_focused = false;
    zorder_remove(w);
    refresh_focus();
}

void wm_minimize(struct window *w) {
    if (!w->open || w->minimized) return;
    comp_damage(wm_bounds(w));
    w->minimized = true;
    if (w->app->focus && w->drawn_focused) w->app->focus(w, false);
    w->drawn_focused = false;
    refresh_focus();
}

struct window *wm_window_at(int x, int y) {
    for (int i = nwindows - 1; i >= 0; i--) {
        struct window *w = zorder[i];
        if (w->open && !w->minimized && rect_contains(w->frame, x, y)) return w;
    }
    return 0;
}

void wm_invalidate(struct window *w, struct rect r) {
    if (!w->open || w->minimized) return;
    r = rect_intersect(r, (struct rect){ 0, 0, w->client.w, w->client.h });
    r.x += w->frame.x;
    r.y += w->frame.y + WIN_TITLE_H;
    comp_damage(r);
}

void wm_invalidate_all(struct window *w) {
    wm_invalidate(w, (struct rect){ 0, 0, w->client.w, w->client.h });
}

// ---- decorations ----
// Three lights on the left: close, minimize, zoom.
static struct rect button_rect(int which) {
    int x = LIGHT_X0 + (which - BTN_CLOSE) * (LIGHT_D + LIGHT_GAP);
    return (struct rect){ x, (WIN_TITLE_H - LIGHT_D) / 2, LIGHT_D, LIGHT_D };
}

static color_t darken(color_t c, int amount) {
    return gfx_lerp_color(c, RGB(0, 0, 0), amount, 255);
}

void wm_paint_titlebar(struct window *w) {
    struct surface *s = &w->surf;
    bool active = (w == wm_focused()) && w->open && !w->minimized;
    gfx_reset_clip(s);
    struct rect bar = { 0, 0, w->frame.w, WIN_TITLE_H - 1 };
    if (active) gfx_vgradient(s, bar, WIN_TITLE_TOP, WIN_TITLE_BOTTOM);
    else gfx_fill(s, bar, WIN_TITLE_IDLE);
    gfx_fill(s, (struct rect){ 0, WIN_TITLE_H - 1, w->frame.w, 1 }, active ? WIN_TITLE_SEP : WIN_TITLE_SEP_IDLE);

    static const color_t colors[] = { 0, LIGHT_CLOSE, LIGHT_MIN, LIGHT_ZOOM };
    bool glyphs = w->hover_btn != BTN_NONE;
    for (int b = BTN_CLOSE; b <= BTN_ZOOM; b++) {
        struct rect r = button_rect(b);
        int cx = (r.x * 2 + r.w) * 8, cy = (r.y * 2 + r.h) * 8, rad = r.w * 8;
        color_t c = (active || glyphs) ? colors[b] : LIGHT_IDLE;
        gfx_disc(s, cx, cy, rad, darken(c, LIGHT_RING_DARKEN));
        gfx_disc(s, cx, cy, rad - 14, c);
        if (w->press_btn == b) gfx_disc(s, cx, cy, rad, RGBA(0, 0, 0, 0x30));
        if (!glyphs) continue;
        int k = 40;   // glyph half-size, 1/16 px
        if (b == BTN_CLOSE) {
            gfx_capsule(s, cx - k + 4, cy - k + 4, cx + k - 4, cy + k - 4, 9, LIGHT_GLYPH);
            gfx_capsule(s, cx - k + 4, cy + k - 4, cx + k - 4, cy - k + 4, 9, LIGHT_GLYPH);
        } else if (b == BTN_MIN) {
            gfx_capsule(s, cx - k, cy, cx + k, cy, 10, LIGHT_GLYPH);
        } else {
            gfx_capsule(s, cx - k, cy, cx + k, cy, 10, LIGHT_GLYPH);
            gfx_capsule(s, cx, cy - k, cx, cy + k, 10, LIGHT_GLYPH);
        }
    }
    text_center(s, &afont_ui_bold, 0, w->frame.w, (WIN_TITLE_H - afont_ui_bold.height) / 2,
                w->app->title ? w->app->title(w) : w->app->name, active ? C_TEXT_2 : C_TEXT_INACTIVE);
    comp_damage((struct rect){ w->frame.x, w->frame.y, w->frame.w, WIN_TITLE_H });
}

int wm_button_at(struct window *w, int x, int y) {
    x -= w->frame.x; y -= w->frame.y;
    for (int b = BTN_CLOSE; b <= BTN_ZOOM; b++) {
        struct rect r = rect_inset(button_rect(b), -(LIGHT_GAP / 2));
        if (rect_contains(r, x, y)) return b;
    }
    return BTN_NONE;
}

bool wm_in_titlebar(struct window *w, int x, int y) {
    return rect_contains((struct rect){ w->frame.x, w->frame.y, w->frame.w, WIN_TITLE_H }, x, y);
}

void wm_move(struct window *w, int x, int y) {
    // Keep the title bar reachable: below the top bar, at least 80 px visible.
    if (y < MENUBAR_H) y = MENUBAR_H;
    if (y > scr.h - WIN_TITLE_H) y = scr.h - WIN_TITLE_H;
    if (x < 80 - w->frame.w) x = 80 - w->frame.w;
    if (x > scr.w - 80) x = scr.w - 80;
    w->frame.x = x;
    w->frame.y = y;
}

void wm_sync_damage(void) {
    for (int i = 0; i < nwindows; i++) {
        struct window *w = zorder[i];
        if (w->drawn.x != w->frame.x || w->drawn.y != w->frame.y) {
            struct rect a = bounds_of(w->drawn), b = wm_bounds(w);
            // Small moves: one union rect; large jumps: two rects.
            if (rect_overlaps(a, b)) comp_damage(rect_union(a, b));
            else { comp_damage(a); comp_damage(b); }
            w->drawn = w->frame;
        }
    }
}

void wm_draw(struct surface *b, struct window *w) {
    bool active = w->drawn_focused;
    gfx_shadow(b, w->frame, WIN_RADIUS, WIN_SHADOW, WIN_SHADOW_Y, active ? WIN_SHADOW_A : WIN_SHADOW_IDLE_A);
    gfx_blit_rounded(b, w->frame.x, w->frame.y, &w->surf, WIN_RADIUS);
    gfx_stroke_rrect(b, w->frame, WIN_RADIUS, active ? WIN_BORDER : WIN_BORDER_IDLE);
}
