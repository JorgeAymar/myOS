// Damage-tracking compositor: a kmalloc'd back buffer, a cached wallpaper and
// a small list of dirty rectangles. Only dirty regions are re-composited and
// copied to the Limine framebuffer (honoring its pitch).
#include "wm.h"
#include "boot.h"
#include "kprintf.h"
#include "mm.h"

struct screen scr;

#define MAX_DIRTY 24
static struct rect dirty[MAX_DIRTY];
static int ndirty;

static long area(struct rect r) { return rect_empty(r) ? 0 : (long)r.w * r.h; }

// Frosted glass, the cheap way: the wallpaper is static, so blur the band
// once (with a margin so edges blend correctly) and pre-apply the tint.
static bool frost_band(struct surface *out, int y0, int h, color_t tint) {
    int m = BLUR_RADIUS * BLUR_PASSES;
    int sy0 = y0 - m < 0 ? 0 : y0 - m, sy1 = y0 + h + m > scr.h ? scr.h : y0 + h + m;
    struct surface tmp;
    if (!gfx_surface_alloc(&tmp, scr.w, sy1 - sy0)) return false;
    uint32_t *line = kmalloc(sizeof(uint32_t) * (size_t)(scr.w > scr.h ? scr.w : scr.h));
    if (!line) return false;
    gfx_blit_rect(&tmp, 0, 0, &scr.wall, (struct rect){ 0, sy0, scr.w, sy1 - sy0 });
    gfx_box_blur(&tmp, BLUR_RADIUS, BLUR_PASSES, line);
    if (!gfx_surface_alloc(out, scr.w, h)) return false;
    gfx_blit_rect(out, 0, 0, &tmp, (struct rect){ 0, y0 - sy0, scr.w, h });
    gfx_blend(out, (struct rect){ 0, 0, scr.w, h }, tint);
    kfree(line);
    kfree(tmp.px);
    return true;
}

bool comp_init(void) {
    struct limine_framebuffer *fb = boot_framebuffer();
    if (fb->bpp != 32) {
        kprintf("[WARN] desktop: unsupported framebuffer bpp %u\n", fb->bpp);
        return false;
    }
    if (fb->red_mask_shift != 16 || fb->green_mask_shift != 8 || fb->blue_mask_shift != 0)
        kprintf("[WARN] desktop: framebuffer is not XRGB8888, colors may be off\n");
    scr.w = (int)fb->width;
    scr.h = (int)fb->height;
    scr.fb = fb->address;
    scr.fb_pitch = (long)fb->pitch;
    if (!gfx_surface_alloc(&scr.back, scr.w, scr.h)) return false;
    if (!gfx_surface_alloc(&scr.wall, scr.w, scr.h)) return false;
    wallpaper_render(&scr.wall);
    if (!frost_band(&scr.frost_top, 0, MENUBAR_H, MENUBAR_TINT)) return false;
    scr.frost_dock_y = scr.h - DOCK_BAND_H;
    if (!frost_band(&scr.frost_dock, scr.frost_dock_y, DOCK_BAND_H, DOCK_TINT)) return false;
    comp_damage_all();
    return true;
}

void comp_damage(struct rect r) {
    r = rect_intersect(r, (struct rect){ 0, 0, scr.w, scr.h });
    if (rect_empty(r)) return;
    // Merge with any rect where the union wastes little area.
    for (int i = 0; i < ndirty; i++) {
        struct rect u = rect_union(dirty[i], r);
        if (area(u) <= area(dirty[i]) + area(r) + 64 * 64 ||
            (rect_overlaps(dirty[i], r) && area(u) * 3 <= (area(dirty[i]) + area(r)) * 4)) {
            r = u;
            dirty[i] = dirty[--ndirty];
            i = -1;   // re-scan: the bigger rect may now merge with others
        }
    }
    if (ndirty == MAX_DIRTY) {
        int best = 0; long best_cost = -1;
        for (int i = 0; i < ndirty; i++) {
            long cost = area(rect_union(dirty[i], r)) - area(dirty[i]);
            if (best_cost < 0 || cost < best_cost) { best = i; best_cost = cost; }
        }
        dirty[best] = rect_union(dirty[best], r);
        return;
    }
    dirty[ndirty++] = r;
}

void comp_damage_all(void) {
    ndirty = 0;
    comp_damage((struct rect){ 0, 0, scr.w, scr.h });
}

static void present(struct rect r) {
    for (int y = r.y; y < r.y + r.h; y++)
        gfx_copy32((uint32_t *)(scr.fb + (long)y * scr.fb_pitch) + r.x,
                   scr.back.px + (long)y * scr.back.stride + r.x, r.w);
}

static void compose(struct rect r) {
    struct surface *b = &scr.back;
    gfx_set_clip(b, r);
    gfx_blit_rect(b, r.x, r.y, &scr.wall, r);
    for (int i = 0; i < nwindows; i++) {
        struct window *w = zorder[i];
        if (!w->open || w->minimized) continue;
        if (rect_overlaps(wm_bounds(w), r)) wm_draw(b, w);
    }
    shell_draw(b);
    if (rect_overlaps(cursor_rect(cursor_x, cursor_y), r)) shell_draw_cursor(b);
    gfx_reset_clip(b);
}

void comp_render(void) {
    // Snapshot: drawing never adds damage, but keep the loop robust anyway.
    struct rect list[MAX_DIRTY];
    int n = ndirty;
    for (int i = 0; i < n; i++) list[i] = dirty[i];
    ndirty = 0;
    for (int i = 0; i < n; i++) compose(list[i]);
    for (int i = 0; i < n; i++) present(list[i]);
}
